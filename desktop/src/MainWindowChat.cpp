
#include "ForkMeshVersion.h"
#include "ProcessReaper.h"
#include "ProjectRegistry.h"
#include "Tooltips.h"
#include "UiSounds.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"
#include "SettingsSectionsView.h"
#include "SystemPressure.h"
#include "AgentCliLocator.h"
#include "AgentLoginPty.h"
#include "ClaudeAccountRouting.h"
#include "SafeDelete.h"
#include "LocalControlAgentOps.h"
#include "AgentWorktree.h"
#include "RepoStatsStore.h"
#include "ActionStore.h"
#include "ControlNode.h"
#include "CurrentPageStack.h"
#include "MirrorSyncVerdict.h"
#include "D1QuerySource.h"
#include "DatabaseManagerWidget.h"
#include "ForkMeshPackageRegistry.h"
#include "KebabHeaderView.h"
#include "LogTimelineChart.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "PromptDraftStore.h"
#include "PromptSpellCheckHighlighter.h"
#include "PublicMirrorRuntime.h"
#include "QtRuntimeDiagnostics.h"
#include "RewardPoolSigner.h"
#include "TerminalWidget.h"
#include "WorldSpeechBridge.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QFormLayout>
#include <QHostAddress>
#include <QInputDialog>
#include <QMouseEvent>
#include <QNetworkInformation>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QUuid>

#include "SectionViewport.h"

#include <algorithm>
#include <cmath>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <signal.h>
#endif

using namespace forkmesh::ui;

namespace {
constexpr qint64 kBackgroundTaskShowAfterMs = forkmesh::kBackgroundShowAfterMs;

class ChatPreviewLabel : public QLabel
{
public:
    struct Line {
        QString who;
        QString text;
        bool mine = false;
        QPixmap avatar;
    };

    static constexpr int kAvatarPx = 16;
    static constexpr int kRowGap = 4;
    static constexpr int kTopPad = 4;
    static constexpr int kBottomPad = 5;

    ChatPreviewLabel()
    {
        setObjectName(QStringLiteral("promptChatPreview"));
        QFont small = font();
        small.setPixelSize(11);
        setFont(small);
        setContentsMargins(10, kTopPad, 10, kBottomPad);
        setTextFormat(Qt::RichText);
        setWordWrap(false);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("The latest messages in the conversation the "
                                  "chat button sends to. Click to open it."));
    }

    void setLines(const QList<Line> &lines)
    {
        m_lines = lines;
        rebuild();
        updateGeometry();
        update();
    }

    bool hasLines() const { return !m_lines.isEmpty(); }
    QList<Line> lines() const { return m_lines; }
    std::function<void()> onClicked;

    int rowHeight() const
    {
        return qMax(kAvatarPx, QFontMetrics(font()).height());
    }

    QSize sizeHint() const override
    {
        const int rows = qMax(1, int(m_lines.size()));
        const QMargins margins = contentsMargins();
        return QSize(QLabel::sizeHint().width(),
                     rows * rowHeight() + (rows - 1) * kRowGap + margins.top() +
                         margins.bottom());
    }

    QSize minimumSizeHint() const override { return QSize(0, sizeHint().height()); }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QFrame::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRect area = contentsRect();
        const QFontMetrics metrics(font());
        QFont bold = font();
        bold.setWeight(QFont::DemiBold);
        const QFontMetrics boldMetrics(bold);
        const QColor accent = Theme::color(Theme::Role::AccentFg);
        const QColor strong = Theme::color(Theme::Role::FgSecondary);
        const QColor body = palette().color(QPalette::WindowText);
        const int rowH = rowHeight();
        int y = area.top();
        for (const Line &line : std::as_const(m_lines)) {
            int x = area.left();
            const QRect face(x, y + (rowH - kAvatarPx) / 2, kAvatarPx, kAvatarPx);
            if (!line.avatar.isNull()) {
                painter.drawPixmap(face, line.avatar);
            } else {
                painter.setPen(Qt::NoPen);
                painter.setBrush(line.mine ? strong : accent);
                painter.drawEllipse(face);
            }
            x += kAvatarPx + 6;
            const QString who = line.who + QStringLiteral(": ");
            const QRect row(x, y, qMax(0, area.right() - x + 1), rowH);
            painter.setFont(bold);
            painter.setPen(line.mine ? strong : accent);
            painter.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, who);
            const int textX = x + boldMetrics.horizontalAdvance(who);
            const int room = qMax(10, area.right() - textX + 1);
            painter.setFont(font());
            painter.setPen(body);
            painter.drawText(QRect(textX, y, room, rowH),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             metrics.elidedText(line.text, Qt::ElideRight, room));
            y += rowH + kRowGap;
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onClicked) {
            onClicked();
            event->accept();
            return;
        }
        QLabel::mouseReleaseEvent(event);
    }

private:
    void rebuild()
    {
        QStringList rows;
        for (const Line &line : std::as_const(m_lines))
            rows << (line.who + QStringLiteral(": ")).toHtmlEscaped() +
                        line.text.toHtmlEscaped();
        const QString html = rows.join(QStringLiteral("<br>"));
        if (text() != html)
            setText(html);
    }

    QList<Line> m_lines;
};

class FloatingPromptBar : public QWidget
{
public:
    static constexpr int kFadePx = 36;

    FloatingPromptBar(QWidget *host, QWidget *spacer, QWidget *preview, int radius)
        : QWidget(host), m_spacer(spacer), m_preview(preview), m_radius(radius)
    {
        host->installEventFilter(this);
        if (preview)
            preview->installEventFilter(this);
    }

    void place()
    {
        QWidget *host = parentWidget();
        if (!host)
            return;
        const int width = qMin(qMax(0, host->width() - 2), sizeHint().width());
        setGeometry(host->width() - 1 - width, 1, width, height());
        raise();
        syncSpacer();
    }

    void setVisible(bool visible) override
    {
        QWidget::setVisible(visible);
        syncSpacer();
    }

protected:
    bool event(QEvent *event) override
    {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show)
            place();
        return handled;
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (watched == parentWidget()) {
            if (type == QEvent::Resize || type == QEvent::Show)
                place();
        } else if (watched == m_preview) {
            if (type == QEvent::Show || type == QEvent::Hide ||
                type == QEvent::ShowToParent || type == QEvent::HideToParent)
                syncSpacer();
        }
        return false;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor surface = Theme::nextOr(
            Theme::Role::Surface,
            Theme::activeDark() ? QColor(QStringLiteral("#0d1117"))
                                : QColor(QStringLiteral("#ffffff")));
        QColor clear = surface;
        clear.setAlpha(0);
        QLinearGradient fade(0, 0, kFadePx, 0);
        fade.setColorAt(0.0, clear);
        fade.setColorAt(1.0, surface);
        const qreal w = width();
        const qreal h = height();
        const qreal r = qMin<qreal>(m_radius, h);
        QPainterPath shape;
        shape.moveTo(0, 0);
        shape.lineTo(w - r, 0);
        shape.arcTo(QRectF(w - 2 * r, 0, 2 * r, 2 * r), 90, -90);
        shape.lineTo(w, h);
        shape.lineTo(0, h);
        shape.closeSubpath();
        painter.fillPath(shape, fade);
    }

private:
    void syncSpacer()
    {
        if (m_spacer)
            m_spacer->setVisible(!isHidden() && (!m_preview || m_preview->isHidden()));
    }

    QPointer<QWidget> m_spacer;
    QPointer<QWidget> m_preview;
    int m_radius = 0;
};

constexpr int kComposerIconPx = 16;
constexpr int kComposerControlPx = 28;
constexpr int kComposerActionWidth = 66;

QIcon composerMonoIcon(const QString &name)
{
    return themedOcticon(name, Theme::color(Theme::Role::FgMuted), kComposerIconPx);
}

QIcon composerModeIcon(int index)
{
    static const char *const kModeOcticons[] = {"unlock", "hand", "eye", "pencil"};
    if (!Theme::isNext() || index < 7 || index > 10)
        return agentControlIcon(index);
    return composerMonoIcon(QString::fromLatin1(kModeOcticons[index - 7]));
}

QIcon composerEffortIcon(int level, int levels)
{
    const int px = kComposerIconPx;
    QPixmap pixmap = crispIconPixmap(px, iconDevicePixelRatio());
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF ring(2.0, 2.0, px - 4.0, px - 4.0);
    QColor track = Theme::color(Theme::Role::FgMuted);
    track.setAlphaF(0.35);
    painter.setPen(QPen(track, 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(ring);
    const qreal fraction =
        levels > 1 ? qBound<qreal>(0.0, qreal(level + 1) / levels, 1.0) : 1.0;
    painter.setPen(QPen(Theme::color(Theme::Role::FgSecondary), 2.0,
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring, 90 * 16, -qRound(fraction * 360.0 * 16.0));
    painter.end();
    return QIcon(pixmap);
}

QString composerProviderOcticon(const QString &provider)
{
    if (provider == QLatin1String("claude-code") || agentIsCodexProvider(provider))
        return QStringLiteral("terminal");
    if (provider == QLatin1String("openai") || provider == QLatin1String("claude-api"))
        return QStringLiteral("server");
    if (agentIsCloudflareAiProvider(provider))
        return QStringLiteral("cloud");
    if (provider == QLatin1String("manual"))
        return QStringLiteral("issue-opened");
    if (provider == kActivityPubPromptProvider)
        return QStringLiteral("mastodon");
    if (provider == kNotePromptProvider)
        return QStringLiteral("note");
    if (provider == kTaskPromptProvider)
        return QStringLiteral("people");
    if (provider == kEditComposerModelsRow)
        return QStringLiteral("gear");
    return QStringLiteral("device-desktop");
}

void setComposerIconTone(QPushButton *button)
{
    if (button)
        button->setProperty("nextIconRole", static_cast<int>(Theme::Role::FgMuted));
}

constexpr int kBackgroundTaskIdleTicksBeforeStop = 12;
constexpr qint64 kBackgroundTaskFastFlushMs = 2000;

QString modelFamilyId(const QString &model)
{
    const QString lower = model.toLower();
    if (lower == QLatin1String("auto"))
        return QStringLiteral("auto");
    static const QRegularExpression kAliasRx(
        QStringLiteral("(opus|sonnet|haiku|fable)"));
    const QRegularExpressionMatch alias = kAliasRx.match(lower);
    return alias.hasMatch() ? alias.captured(1) : QString();
}

QString quickAddUsageLimitCountdownText(const QString &provider)
{
    const QString providerKey =
        provider == QLatin1String("claude-code") || provider.startsWith(QLatin1String("claude"))
            ? QStringLiteral("claude")
            : (provider == kCodexProvider
                   ? QStringLiteral("codex")
                   : QString());
    if (providerKey.isEmpty())
        return QString();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSettings settings;
    QString bestLabel;
    qint64 bestRemaining = 0;

    auto exhaustedSetting = [](const QString &providerKey,
                              const QString &windowKey) -> QString {
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
    };
    auto resetSetting = [](const QString &providerKey,
                           const QString &windowKey) -> QString {
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
    };

    auto considerWindow = [&](const QString &windowKey, const QString &label) {
        const QString exhaustedKey = exhaustedSetting(providerKey, windowKey);
        if (exhaustedKey.isEmpty() || !settings.value(exhaustedKey).toBool())
            return;
        const QString resetKey = resetSetting(providerKey, windowKey);
        const qint64 resetAt = settings.value(resetKey).toLongLong();
        if (resetAt <= 0)
            return;
        const qint64 remaining = resetAt - now;
        if (remaining <= 0)
            return;
        if (bestRemaining == 0 || remaining < bestRemaining) {
            bestRemaining = remaining;
            bestLabel = label;
        }
    };

    considerWindow(QStringLiteral("5h"), QStringLiteral("5-hour"));
    considerWindow(QStringLiteral("weekly"), QStringLiteral("weekly"));
    if (providerKey == QLatin1String("claude"))
        considerWindow(QStringLiteral("fable"), QStringLiteral("Fable weekly"));

    if (bestRemaining <= 0)
        return QString();
    return QStringLiteral("%1 usage limit reached · resets in %2")
        .arg(bestLabel, humanizeRemaining(bestRemaining));
}

QString quickAddUsageLimitRemainingText(const QString &provider)
{
    const QString countdown = quickAddUsageLimitCountdownText(provider);
    static const QString marker = QStringLiteral("resets in ");
    const int markerIdx = countdown.indexOf(marker);
    return markerIdx >= 0 ? countdown.mid(markerIdx + marker.size()).trimmed()
                          : countdown;
}

struct ModelChoiceOutcome {
    QString state;  // empty when nothing has finished on this model yet
    QString detail;
};

QString quickAddModelChoiceWhen(const AgentSession &session)
{
    return session.finishedAtMs > 0
               ? QStringLiteral(" %1").arg(
                     formatIssueRelativeTime(session.finishedAtMs))
               : QString();
}

ModelChoiceOutcome quickAddModelChoiceSuccess(const AgentSession &session)
{
    return {kAgentChoiceStatusOk,
            QStringLiteral("Last run on this model succeeded%1")
                .arg(quickAddModelChoiceWhen(session))};
}

ModelChoiceOutcome quickAddModelChoiceFailure(const AgentSession &session)
{
    QString detail = (session.status == AgentStatus::Stopped
                          ? QStringLiteral("Last run on this model was stopped%1")
                          : QStringLiteral("Last run on this model failed%1"))
                         .arg(quickAddModelChoiceWhen(session));
    const QString message = session.lastError.simplified();
    if (!message.isEmpty())
        detail += QStringLiteral(": %1").arg(message);
    return {kAgentChoiceStatusFailed, detail};
}

QString maskCredentialValue(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return QString();
    if (trimmed.size() <= 10)
        return QStringLiteral("••••");
    return QStringLiteral("%1…%2").arg(trimmed.left(4), trimmed.right(4));
}

QString firstNonEmpty(const QStringList &values)
{
    for (const QString &value : values) {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty())
            return trimmed;
    }
    return QString();
}

QString jsonString(const QJsonObject &obj, const QString &key)
{
    return obj.value(key).toString().trimmed();
}

QString jsonString(const QJsonObject &obj, const QString &container,
                  const QString &key)
{
    const QJsonObject nested = obj.value(container).toObject();
    return nested.value(key).toString().trimmed();
}

QHash<QString, QString> &claudeCliIdentityCache()
{
    static QHash<QString, QString> cache;
    return cache;
}

QSet<QString> &claudeCliIdentityProbes()
{
    static QSet<QString> probes;
    return probes;
}

QString claudeCliIdentityFromStatus(const QByteArray &statusJson)
{
    const QJsonObject auth = QJsonDocument::fromJson(statusJson).object();
    return firstNonEmpty({jsonString(auth, QStringLiteral("name")),
                          jsonString(auth, QStringLiteral("username"))});
}

QString agentCliIdentityLabel(const QString &provider,
                              const QString &configDir = QString())
{
    const bool codex = agentIsCodexProvider(provider);
    const QString filePath = agentAccountCredentialPath(
        provider, configDir.isEmpty() ? agentAccountDefaultConfigDir(provider)
                                      : configDir);
    const auto readFile = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return file.readAll();
    };

    const QJsonObject credentials =
        QJsonDocument::fromJson(readFile(filePath)).object();

    QString name;
    QString token;

    // Claude's credentials file intentionally contains OAuth material rather
    // than the human identity. showAgentAccountMenu() probes the CLI
    // asynchronously and fills this process cache; ordinary model-picker
    // refreshes only read the cache/files and can never wait on a subprocess.
    if (!codex) {
        const QString defaultDir = agentAccountDefaultConfigDir(provider);
        const QString dir = configDir.isEmpty() ? defaultDir : configDir;
        const bool homeState =
            qEnvironmentVariable("CLAUDE_CONFIG_DIR").trimmed().isEmpty() &&
            QDir::cleanPath(dir) == QDir::cleanPath(defaultDir);
        const QString displayName =
            forkmesh::agentaccounts::readClaudeLoginIdentity(
                forkmesh::agentaccounts::claudeStateFile(dir, homeState))
                .displayName;
        if (!displayName.isEmpty())
            return displayName;
        const QString cached =
            claudeCliIdentityCache().value(QDir::cleanPath(configDir));
        if (!cached.isEmpty())
            return cached;
    }

    if (codex) {
        const QJsonObject tokens = credentials.value(QStringLiteral("tokens")).toObject();
        token = firstNonEmpty({
            jsonString(tokens, QStringLiteral("openAiApiKey")),
            jsonString(tokens, QStringLiteral("access_token")),
            jsonString(tokens, QStringLiteral("apiKey")),
            jsonString(tokens, QStringLiteral("token")),
            jsonString(credentials, QStringLiteral("access_token")),
            jsonString(credentials, QStringLiteral("apiKey")),
            jsonString(credentials, QStringLiteral("token")),
            qEnvironmentVariable("OPENAI_API_KEY"),
            QSettings().value(kCodexApiKeySetting).toString(),
            QSettings().value(kOpenAiAdminKeySetting).toString(),
        });
        name = firstNonEmpty({
            jsonString(credentials, QStringLiteral("account"), QStringLiteral("name")),
            jsonString(credentials, QStringLiteral("user"), QStringLiteral("name")),
            jsonString(credentials, QStringLiteral("name")),
            jsonString(credentials, QStringLiteral("user")),
        });
    } else {
        const QJsonObject oauth =
            credentials.value(QStringLiteral("claudeAiOauth")).toObject();
        name = firstNonEmpty({
            name,
            jsonString(oauth, QStringLiteral("user"), QStringLiteral("name")),
            jsonString(oauth, QStringLiteral("user"), QStringLiteral("username")),
            jsonString(oauth, QStringLiteral("account"), QStringLiteral("name")),
            jsonString(oauth, QStringLiteral("name")),
            jsonString(oauth, QStringLiteral("user")),
            jsonString(credentials, QStringLiteral("name")),
        });
        token = firstNonEmpty({
            jsonString(oauth, QStringLiteral("accessToken")),
            jsonString(oauth, QStringLiteral("refreshToken")),
            jsonString(oauth, QStringLiteral("apiKey")),
            jsonString(oauth, QStringLiteral("access_token")),
            qEnvironmentVariable("CLAUDE_CODE_OAUTH_TOKEN"),
            qEnvironmentVariable("ANTHROPIC_AUTH_TOKEN"),
            qEnvironmentVariable("ANTHROPIC_API_KEY"),
            QSettings().value(kClaudeApiKeySetting).toString(),
            QSettings().value(kClaudeAdminKeySetting).toString(),
        });
    }

    if (!name.isEmpty())
        return name;
    if (!token.isEmpty())
        return QStringLiteral("token %1").arg(maskCredentialValue(token));
    return QString();
}

bool providerAccountSignedIn(const QString &provider, const QString &configDir)
{
    QFile file(agentAccountCredentialPath(provider, configDir));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject credentials =
        QJsonDocument::fromJson(file.readAll()).object();
    if (agentIsCodexProvider(provider)) {
        return !credentials.value(QStringLiteral("tokens")).toObject().isEmpty() ||
               !credentials.value(QStringLiteral("access_token")).toString().isEmpty() ||
               !credentials.value(QStringLiteral("OPENAI_API_KEY")).toString().isEmpty();
    }
    const QJsonObject oauth =
        credentials.value(QStringLiteral("claudeAiOauth")).toObject();
    return !oauth.value(QStringLiteral("accessToken")).toString().isEmpty() ||
           !oauth.value(QStringLiteral("refreshToken")).toString().isEmpty();
}

ModelChoiceOutcome quickAddModelChoiceSummary(const QList<AgentSession> &sessions,
                                             const QString &provider,
                                             const QString &choiceModel)
{
    const QString targetModel = choiceModel.trimmed().toLower();
    if (targetModel.isEmpty())
        return {};

    const QString targetFamily = modelFamilyId(targetModel);
    const auto statusIsTerminal =
        [](const QString &status) {
            return status == AgentStatus::Success || status == AgentStatus::Failed ||
                status == AgentStatus::Stopped;
        };
    const auto endedAtMs = [](const AgentSession &session) {
        if (session.finishedAtMs > 0)
            return session.finishedAtMs;
        return session.startedAtMs > 0 ? session.startedAtMs
                                       : session.createdAtMs;
    };

    const AgentSession *exact = nullptr;
    const AgentSession *family = nullptr;
    for (const AgentSession &session : sessions) {
        if (session.provider != provider || !statusIsTerminal(session.status))
            continue;
        const QString model = session.model.trimmed().toLower();
        if (model.isEmpty())
            continue;
        const bool sameModel = model == targetModel;
        const bool sameFamily = !targetFamily.isEmpty() &&
                                modelFamilyId(model) == targetFamily;
        if (!sameModel && !sameFamily)
            continue;
        const AgentSession *&newest = sameModel ? exact : family;
        if (!newest || endedAtMs(session) > endedAtMs(*newest))
            newest = &session;
    }

    const AgentSession *latest = exact ? exact : family;
    if (!latest)
        return {};
    return latest->status == AgentStatus::Success
               ? quickAddModelChoiceSuccess(*latest)
               : quickAddModelChoiceFailure(*latest);
}
} // namespace

namespace forkmesh::ui {

bool agentAccountSignedIn(const QString &provider, const QString &configDir)
{
    return providerAccountSignedIn(provider, configDir);
}

QString agentAccountIdentity(const QString &provider, const QString &configDir)
{
    return agentCliIdentityLabel(provider, configDir);
}

} // namespace forkmesh::ui

namespace {
QString usagePaceNote(int usedPercent, qint64 resetAtMs, qint64 windowMs,
                      qint64 now)
{
    if (usedPercent < 0 || windowMs <= 0 || resetAtMs <= now)
        return QString();
    const qint64 elapsed =
        qBound<qint64>(0, windowMs - (resetAtMs - now), windowMs);
    const int ahead =
        usedPercent - qRound(elapsed * 100.0 / double(windowMs));
    if (ahead == 0)
        return QStringLiteral("on pace");
    return QStringLiteral("%1% %2 pace")
        .arg(qAbs(ahead))
        .arg(ahead > 0 ? QStringLiteral("over") : QStringLiteral("under"));
}

} // namespace

QList<forkmesh::ui::AgentUsageMenuData>
forkmesh::ui::agentAccountUsageMenuData(const QString &provider,
                                        const QString &accountId)
{
    const bool codex = agentIsCodexProvider(provider);
    const bool builtIn = accountId == QLatin1String("default");
    QSettings settings;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto value = [&](const QString &globalKey) -> QVariant {
        const QString scoped = agentAccountUsageSetting(
            provider, accountId, agentAccountUsageField(globalKey));
        if (settings.contains(scoped))
            return settings.value(scoped);
        return builtIn ? settings.value(globalKey) : QVariant();
    };
    auto line = [&](const QString &label, const QString &pctKey,
                    const QString &resetKey, qint64 windowMs,
                    const QString &anchorKey = QString(),
                    qint64 durationMs = 0) {
        const QVariant pctValue = value(pctKey);
        QString amount = QString::fromUtf8("\xE2\x80\x94");
        QString availability;
        int usedPercent = -1;
        qint64 estimatedReset = 0;
        if (pctValue.isValid()) {
            const int used = qBound(0, pctValue.toInt(), 100);
            amount = QStringLiteral("%1% used").arg(used);
            usedPercent = used;
            if (codex)
                availability = QStringLiteral("%1% available").arg(100 - used);
        } else if (codex && !anchorKey.isEmpty()) {
            const qint64 anchor = value(anchorKey).toLongLong();
            const qint64 remaining = durationMs - (now - anchor);
            if (anchor > 0 && remaining > 0) {
                const int remainingPercent =
                    qBound(0, qRound(remaining * 100.0 / double(durationMs)),
                           100);
                amount = QStringLiteral("usage unavailable");
                availability = QStringLiteral("%1% of window remaining")
                                   .arg(remainingPercent);
                estimatedReset = anchor + durationMs;
            }
        }
        QString resetNote;
        const qint64 providerReset = value(resetKey).toLongLong();
        const qint64 resetAt = providerReset > 0 ? providerReset : estimatedReset;
        if (resetAt > now)
            resetNote = QStringLiteral("resets in %1")
                            .arg(humanizeRemaining(resetAt - now));
        QStringList notes;
        if (!availability.isEmpty())
            notes << availability;
        if (!resetNote.isEmpty())
            notes << resetNote;
        const QString pace = usagePaceNote(usedPercent, resetAt, windowMs, now);
        if (!pace.isEmpty())
            notes << pace;
        const QString note = notes.join(QString::fromUtf8(" \xC2\xB7 "));
        const QString detail = QStringLiteral("%1: %2%3")
                                   .arg(label, amount,
                                        note.isEmpty()
                                            ? QString()
                                            : QString::fromUtf8(" \xC2\xB7 ") +
                                                  note);
        return AgentUsageMenuData{label, amount, note, detail, usedPercent,
                                  resetAt > now ? resetAt : 0, windowMs};
    };

    QList<AgentUsageMenuData> result;
    if (codex) {
        const QJsonArray windows =
            codexUsageWindows(value(kCodexUsageWindowsSetting));
        for (const QJsonValue &windowValue : windows) {
            const QJsonObject window = windowValue.toObject();
            const QString label =
                window.value(QStringLiteral("label")).toString();
            if (label.isEmpty())
                continue;
            int used = qBound(
                0, window.value(QStringLiteral("usedPercent")).toInt(), 100);
            const qint64 resetAt = static_cast<qint64>(
                window.value(QStringLiteral("resetAt")).toDouble());
            const qint64 windowMs =
                static_cast<qint64>(
                    window.value(QStringLiteral("windowDurationMins"))
                        .toDouble()) *
                60 * 1000;
            QString resetNote;
            if (resetAt > now) {
                resetNote = QStringLiteral("resets in %1")
                                .arg(humanizeRemaining(resetAt - now));
            } else if (resetAt > 0) {
                used = 0;
                resetNote = QStringLiteral("ready");
            }
            const QString amount = QStringLiteral("%1% used").arg(used);
            QStringList notes{
                QStringLiteral("%1% available").arg(100 - used)};
            if (!resetNote.isEmpty())
                notes.append(resetNote);
            const QString pace = usagePaceNote(used, resetAt, windowMs, now);
            if (!pace.isEmpty())
                notes.append(pace);
            const QString note =
                notes.join(QString::fromUtf8(" \xC2\xB7 "));
            const QString detail =
                QStringLiteral("%1: %2").arg(label, amount) +
                QString::fromUtf8(" \xC2\xB7 ") + note;
            result.append({label, amount, note, detail, used,
                           resetAt > now ? resetAt : 0, windowMs});
        }
        if (!result.isEmpty())
            return result;
    }

    result << line(QStringLiteral("5-hour"),
                   codex ? kCodexUsage5hPctSetting : kClaudeUsage5hPctSetting,
                   codex ? kCodexUsage5hResetSetting : kClaudeUsage5hResetSetting,
                   kAgentLimit5hMs,
                   codex ? kCodexLimit5hStartSetting : QString(),
                   codex ? kAgentLimit5hMs : 0)
           << line(QStringLiteral("Weekly"),
                   codex ? kCodexUsageWeekPctSetting : kClaudeUsageWeekPctSetting,
                   codex ? kCodexUsageWeekResetSetting : kClaudeUsageWeekResetSetting,
                   kAgentLimitWeekMs,
                   codex ? kCodexLimitWeekStartSetting : QString(),
                   codex ? kAgentLimitWeekMs : 0);
    if (!codex) {
        result << line(QStringLiteral("Fable weekly"),
                       kClaudeUsageFablePctSetting,
                       kClaudeUsageFableResetSetting, kAgentLimitWeekMs);
        // Credits are money rather than a share of a window, so the row is
        // built by hand: the bar tracks the monthly limit, the value line
        // carries the amounts. Absent settings mean the account has no
        // credit balance to speak of, and the row is left out entirely.
        const QVariant creditsEnabled = value(kClaudeCreditsEnabledSetting);
        if (creditsEnabled.isValid()) {
            const QVariant used = value(kClaudeCreditsUsedSetting);
            const QVariant limit = value(kClaudeCreditsLimitSetting);
            const QVariant pct = value(kClaudeCreditsPctSetting);
            const QString currency =
                value(kClaudeCreditsCurrencySetting).toString();
            QString amount = QString::fromUtf8("\xE2\x80\x94");
            QStringList notes;
            if (used.isValid()) {
                const QString spent =
                    formatUsageCredits(used.toDouble(), currency);
                if (limit.isValid()) {
                    amount = QStringLiteral("%1 of %2 used")
                                 .arg(spent, formatUsageCredits(
                                                 limit.toDouble(), currency));
                    notes << QStringLiteral("%1 available")
                                 .arg(formatUsageCredits(
                                     qMax(0.0, limit.toDouble() -
                                                   used.toDouble()),
                                     currency));
                } else {
                    amount = QStringLiteral("%1 used").arg(spent);
                    notes << QStringLiteral("no monthly limit");
                }
            }
            if (!creditsEnabled.toBool()) {
                amount = QStringLiteral("off");
                notes.clear();
                // The second line is painted, not elided, in a ~240px strip,
                // so it has to stay short enough to survive the clip.
                notes << QStringLiteral("enable in Claude settings");
            }
            const QString note = notes.join(QString::fromUtf8(" \xC2\xB7 "));
            const QString label = QStringLiteral("Credits");
            const QString detail =
                QStringLiteral("%1: %2%3")
                    .arg(label, amount,
                         note.isEmpty()
                             ? QString()
                             : QString::fromUtf8(" \xC2\xB7 ") + note);
            result.append({label, amount, note, detail,
                           creditsEnabled.toBool() && pct.isValid()
                               ? qBound(0, pct.toInt(), 100)
                               : -1});
        }
    }
    return result;
}

QList<forkmesh::localcontrol::agentops::UsageWindow>
forkmesh::ui::agentAccountUsageWindows(const QString &provider, const QString &accountId)
{
    QList<forkmesh::localcontrol::agentops::UsageWindow> windows;
    for (const AgentUsageMenuData &row : agentAccountUsageMenuData(provider, accountId)) {
        if (row.label == QLatin1String("Credits"))
            continue;
        forkmesh::localcontrol::agentops::UsageWindow window;
        window.id = row.label.toLower().replace(QLatin1Char(' '), QLatin1Char('-'));
        window.label = row.label;
        window.usedPercent = row.usedPercent;
        window.resetAtMs = row.resetAtMs;
        window.windowMs = row.windowMs;
        windows.append(window);
    }
    return windows;
}

QStringList MainWindow::agentAccountUsageLines(const QString &provider,
                                               const QString &accountId) const
{
    QStringList result;
    for (const AgentUsageMenuData &row :
         agentAccountUsageMenuData(provider, accountId))
        result << row.detail;
    return result;
}

void MainWindow::showAgentAccountMenu(const QString &provider,
                                      const QPoint &globalPosition)
{
    const bool codex = agentIsCodexProvider(provider);
    const QString providerName =
        codex ? QStringLiteral("Codex") : QStringLiteral("Claude Code");
    if (codex)
        refreshCodexUsageRemaining();
    else
        refreshClaudeCodeUsage();

    auto *menu = new QMenu(this);
    menu->setObjectName(codex ? QStringLiteral("codexAccountUsageMenu")
                              : QStringLiteral("claudeAccountUsageMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // Remembered so the usage refreshes below do not have to walk the whole
    // window looking for it. WA_DeleteOnClose clears the pointer for us.
    (codex ? m_codexAccountUsageMenu : m_claudeAccountUsageMenu) = menu;
    QAction *heading = menu->addAction(
        QStringLiteral("%1 accounts and usage").arg(providerName));
    heading->setEnabled(false);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    addAgentUsagePageAction(menu);
    menu->addSeparator();

    if (!codex)
        reconcileClaudeAccountLogins();
    const AgentAccountProfile active = activeAgentAccount(provider);
    const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
    for (const AgentAccountProfile &profile : profiles) {
        const QString identity = agentCliIdentityLabel(provider, profile.configDir);
        const bool signedIn = providerAccountSignedIn(provider, profile.configDir);
        const QString baseLabel = profile.label.trimmed();
        QString label = baseLabel;
        if (!identity.isEmpty())
            label += QStringLiteral(" — ") + identity;
        if (!signedIn)
            label += QString::fromUtf8(" \xC2\xB7 not signed in");
        QAction *account = menu->addAction(label);
        account->setObjectName(QStringLiteral("agentAccount_%1").arg(profile.id));
        account->setCheckable(true);
        account->setChecked(profile.id == active.id);
        account->setToolTip(QStringLiteral("Use this %1 account for new agents")
                                .arg(providerName));
        if (!codex && signedIn && identity.isEmpty())
            probeClaudeAgentAccountIdentity(profile.configDir, account,
                                            baseLabel);
        connect(account, &QAction::triggered, this,
                [this, provider, id = profile.id] {
                    selectAgentAccount(provider, id);
                });
        auto *controls = new QWidget(menu);
        auto *controlsLayout = new QHBoxLayout(controls);
        controlsLayout->setContentsMargins(28, 0, 2, 0);
        controlsLayout->setSpacing(2);
        const QPointer<QMenu> guardedMenu(menu);
        auto addControl = [&](const QString &objectName, const QString &text,
                              const QString &toolTip, bool enabled,
                              std::function<void()> run) {
            auto *button = new QToolButton(controls);
            button->setObjectName(objectName.arg(profile.id));
            button->setAutoRaise(true);
            button->setCursor(Qt::PointingHandCursor);
            button->setText(text);
            button->setToolTip(toolTip);
            button->setAccessibleName(toolTip);
            button->setEnabled(enabled);
            connect(button, &QToolButton::clicked, this,
                    [guardedMenu, run = std::move(run)] {
                        if (guardedMenu)
                            guardedMenu->close();
                        run();
                    });
            controlsLayout->addWidget(button);
            return button;
        };
        addControl(QStringLiteral("agentAccountEdit_%1"),
                   QString::fromUtf8("Edit account\xE2\x80\xA6"),
                   QStringLiteral("Change this account's label"), true,
                   [this, provider, id = profile.id] {
                       editAgentAccount(provider, id);
                   });
        addControl(QStringLiteral("agentAccountLogin_%1"),
                   QString::fromUtf8("Log in\xE2\x80\xA6"),
                   QStringLiteral("Sign this %1 account in from a system terminal")
                       .arg(providerName),
                   true,
                   [this, provider, id = profile.id] {
                       launchAgentSystemTerminal(provider,
                                                 QStringLiteral("login"), id);
                   });
        addControl(QStringLiteral("agentAccountLogout_%1"),
                   QString::fromUtf8("Log out\xE2\x80\xA6"),
                   QStringLiteral("Sign this %1 account out from a system terminal")
                       .arg(providerName),
                   signedIn,
                   [this, provider, id = profile.id] {
                       launchAgentSystemTerminal(provider,
                                                 QStringLiteral("logout"), id);
                   });
        addControl(QStringLiteral("agentAccountDelete_%1"),
                   QString::fromUtf8("Delete\xE2\x80\xA6"),
                   profile.builtIn
                       ? QStringLiteral("The default %1 account is the one in "
                                        "your home folder and cannot be deleted")
                             .arg(providerName)
                       : QStringLiteral("Delete this %1 account from this desktop")
                             .arg(providerName),
                   !profile.builtIn,
                   [this, provider, id = profile.id] {
                       confirmDeleteAgentAccount(provider, id);
                   });
        controlsLayout->addStretch(1);
        QToolButton *launch = addControl(
            QStringLiteral("agentAccountTerminal_%1"), QString(),
            QStringLiteral("Launch %1 in a system terminal as this account")
                .arg(providerName),
            true, [this, provider, id = profile.id] {
                launchAgentSystemTerminal(provider, QStringLiteral("agent"), id);
            });
        launch->setFixedSize(22, 22);
        launch->setIcon(forkmesh::ui::themedOcticon(
            QStringLiteral("terminal"),
            menu->palette().color(QPalette::PlaceholderText), 14));
        auto *controlsAction = new QWidgetAction(menu);
        controlsAction->setObjectName(
            QStringLiteral("agentAccountControls_%1").arg(profile.id));
        controlsAction->setText(QString::fromUtf8("Edit account\xE2\x80\xA6"));
        controlsAction->setDefaultWidget(controls);
        menu->addAction(controlsAction);
        for (const AgentUsageMenuData &usage :
             agentAccountUsageMenuData(provider, profile.id)) {
            auto *row = new TokenUsageMenuRow(usage.label, usage.value,
                                              usage.resetNote, usage.usedPercent,
                                              menu, usage.resetAtMs,
                                              usage.windowMs);
            const QString usageKey =
                QStringLiteral("%1_%2").arg(
                    profile.id, usage.label.toLower().replace('-', '_'));
            if (m_timersFeature) {
                // A countdown on the top-bar clock to the moment this window
                // resets. It reads the row's reset at click time, so a menu
                // left open and refreshed still starts from the live value.
                auto *timerButton = new QToolButton;
                timerButton->setObjectName(
                    QStringLiteral("agentUsageTimer_%1").arg(usageKey));
                timerButton->setAutoRaise(true);
                timerButton->setCursor(Qt::PointingHandCursor);
                timerButton->setIcon(forkmesh::ui::themedOcticon(
                    QStringLiteral("stopwatch"),
                    menu->palette().color(QPalette::PlaceholderText), 14));
                timerButton->setToolTip(
                    QStringLiteral("Add a countdown to the top bar for when "
                                   "%1 resets")
                        .arg(usage.label));
                timerButton->setAccessibleName(
                    QStringLiteral("Add %1 reset timer").arg(usage.label));
                QString timerLabel =
                    QStringLiteral("%1 %2").arg(
                        codex ? QStringLiteral("Codex")
                              : QStringLiteral("Claude"),
                        usage.label);
                if (profiles.size() > 1 && !profile.label.trimmed().isEmpty())
                    timerLabel += QString::fromUtf8(" \xC2\xB7 ") +
                                  profile.label.trimmed();
                const QPointer<TokenUsageMenuRow> guardedRow(row);
                const QPointer<QMenu> guardedMenu(menu);
                connect(timerButton, &QToolButton::clicked, this,
                        [this, guardedRow, guardedMenu, timerLabel] {
                            if (!guardedRow)
                                return;
                            if (addUsageResetCountdown(
                                    timerLabel, guardedRow->resetAtMs()) &&
                                guardedMenu)
                                guardedMenu->close();
                        });
                row->setActionButton(timerButton);
            }
            auto *usageAction = new QWidgetAction(menu);
            usageAction->setText(usage.detail);
            usageAction->setObjectName(
                QStringLiteral("agentUsage_%1").arg(usageKey));
            usageAction->setProperty("agentAccountId", profile.id);
            usageAction->setProperty("agentUsageLabel", usage.label);
            usageAction->setDefaultWidget(row);
            menu->addAction(usageAction);
        }
        menu->addSeparator();
    }

    QAction *add = menu->addAction(
        QString::fromUtf8("Add account\xE2\x80\xA6"));
    add->setObjectName(QStringLiteral("agentAccountAddAction"));
    connect(add, &QAction::triggered, this,
            [this, provider] { addAgentAccount(provider); });
    menu->addSeparator();
    QAction *terminal = menu->addAction(
        QStringLiteral("Launch %1 in system terminal").arg(providerName));
    terminal->setObjectName(QStringLiteral("agentAccountTerminalAction"));
    connect(terminal, &QAction::triggered, this, [this, provider] {
        launchAgentSystemTerminal(provider);
    });
    menu->addSeparator();
    QMenu *runnerMenu = menu->addMenu(QStringLiteral("Runner accounts"));
    runnerMenu->setObjectName(QStringLiteral("agentRunnerAccountsMenu"));
    runnerMenu->menuAction()->setToolTip(
        QStringLiteral("Choose which %1 account each runner uses for cloud "
                       "agents")
            .arg(providerName));
    fillAgentRunnerAccountsMenu(runnerMenu, provider);
    connect(runnerMenu, &QMenu::aboutToShow, this,
            [this, runnerMenu, provider] {
                fillAgentRunnerAccountsMenu(runnerMenu, provider);
            });
    refreshRunners();

    menu->popup(globalPosition);
}

void MainWindow::showAgentUsageMenu(const QPoint &globalPosition)
{
    refreshClaudeCodeUsage();
    refreshCodexUsageRemaining();
    reconcileClaudeAccountLogins();

    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("agentUsageMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QColor iconColor = menu->palette().color(QPalette::PlaceholderText);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    if (addAgentUsagePageAction(menu))
        menu->addSeparator();

    QStringList providers{kClaudeCodeProvider, kCodexProvider};
    if (agentIsCodexProvider(m_agentUsageProvider))
        std::swap(providers[0], providers[1]);
    for (const QString &provider : providers) {
        const bool codex = agentIsCodexProvider(provider);
        const bool shown = provider == providers.first();
        const QString providerName =
            codex ? QStringLiteral("Codex") : QStringLiteral("Claude Code");
        const QString key = codex ? QStringLiteral("Codex") : QStringLiteral("Claude");
        const AgentAccountProfile active = activeAgentAccount(provider);
        const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
        QString slot = key;
        for (int i = 0; i < profiles.size(); ++i)
            if (profiles.at(i).id == active.id)
                slot = QStringLiteral("%1 %2").arg(key).arg(i + 1);

        QAction *heading = menu->addAction(
            providerName + dot + slot +
            (shown ? QStringLiteral(" (on the button)") : QString()));
        heading->setObjectName(QStringLiteral("agentUsageHeading%1").arg(key));
        heading->setEnabled(false);
        QFont headingFont = heading->font();
        headingFont.setBold(true);
        heading->setFont(headingFont);

        for (const AgentUsageMenuData &usage :
             agentAccountUsageMenuData(provider, active.id)) {
            auto *row = new TokenUsageMenuRow(usage.label, usage.value,
                                              usage.resetNote, usage.usedPercent,
                                              menu, usage.resetAtMs,
                                              usage.windowMs);
            auto *usageAction = new QWidgetAction(menu);
            usageAction->setText(usage.detail);
            usageAction->setObjectName(
                QStringLiteral("agentUsageRow%1_%2")
                    .arg(key, usage.label.toLower().replace('-', '_')));
            usageAction->setDefaultWidget(row);
            menu->addAction(usageAction);
        }

        QAction *terminal = menu->addAction(
            forkmesh::ui::themedOcticon(QStringLiteral("terminal"), iconColor, 14),
            QStringLiteral("Open %1 in a terminal").arg(slot));
        terminal->setObjectName(QStringLiteral("agentUsageTerminal%1").arg(key));
        connect(terminal, &QAction::triggered, this, [this, codex] {
            if (codex)
                openAgentProviderTerminal(QStringLiteral("codex"),
                                          QStringLiteral("Codex"),
                                          QStringLiteral("agentCodexTerminalDialog"),
                                          QStringLiteral("agentCodexTerminal"));
            else
                openAgentProviderTerminal(QStringLiteral("claude"),
                                          QStringLiteral("Claude Code"),
                                          QStringLiteral("agentClaudeTerminalDialog"),
                                          QStringLiteral("agentClaudeTerminal"));
        });

        QMenu *accounts = menu->addMenu(
            forkmesh::ui::themedOcticon(QStringLiteral("person"), iconColor, 14),
            QStringLiteral("Run new agents as"));
        accounts->setObjectName(QStringLiteral("agentUsageAccounts%1").arg(key));
        auto *accountGroup = new QActionGroup(accounts);
        accountGroup->setExclusive(true);
        for (int i = 0; i < profiles.size(); ++i) {
            const AgentAccountProfile &profile = profiles.at(i);
            QString label = QStringLiteral("%1 %2").arg(key).arg(i + 1);
            if (!profile.label.trimmed().isEmpty())
                label += dot + profile.label.trimmed();
            QAction *account = accounts->addAction(label);
            account->setObjectName(
                QStringLiteral("agentUsageAccount%1_%2").arg(key, profile.id));
            account->setCheckable(true);
            account->setChecked(profile.id == active.id);
            accountGroup->addAction(account);
            connect(account, &QAction::triggered, this,
                    [this, provider, id = profile.id] {
                        selectAgentAccount(provider, id);
                    });
        }

        QAction *manage = menu->addAction(
            forkmesh::ui::themedOcticon(QStringLiteral("gear"), iconColor, 14),
            QString::fromUtf8("Manage %1 accounts\xE2\x80\xA6").arg(providerName));
        manage->setObjectName(QStringLiteral("agentUsageManage%1").arg(key));
        connect(manage, &QAction::triggered, this, [this, provider, globalPosition] {
            QTimer::singleShot(0, this, [this, provider, globalPosition] {
                showAgentAccountMenu(provider, globalPosition);
            });
        });

        if (!shown) {
            QAction *makeDefault = menu->addAction(
                QStringLiteral("Use %1 for new agents").arg(providerName));
            makeDefault->setObjectName(
                QStringLiteral("agentUsageMakeDefault%1").arg(key));
            makeDefault->setToolTip(
                QStringLiteral("Make %1 the default agent provider, and show its "
                               "usage on the button")
                    .arg(providerName));
            connect(makeDefault, &QAction::triggered, this, [this, provider] {
                QSettings().setValue(kDefaultAgentProviderSetting, provider);
                if (m_defaultAgentProviderCombo) {
                    const QSignalBlocker blocker(m_defaultAgentProviderCombo);
                    selectDefaultAgentProvider(m_defaultAgentProviderCombo);
                }
                selectDefaultAgentProvider(m_quickAddAgentProvider);
                selectDefaultAgentProvider(m_issueAgentProvider);
                selectDefaultAgentProvider(m_issuePrioritizeAgentCombo);
                refreshAgentToolbarAccountButtons();
            });
        }
        menu->addSeparator();
    }

    menu->popup(globalPosition);
}

QAction *MainWindow::addAgentUsagePageAction(QMenu *menu)
{
    QAction *page = menu->addAction(
        forkmesh::ui::themedOcticon(
            QStringLiteral("graph"),
            menu->palette().color(QPalette::PlaceholderText), 14),
        QStringLiteral("Open the Usage page"));
    page->setObjectName(QStringLiteral("agentUsagePageAction"));
    page->setToolTip(QStringLiteral("Every Claude Code and Codex account on this "
                                    "desktop, with its usage, terminals and "
                                    "account controls"));
    connect(page, &QAction::triggered, this, [this] { openAgentUsagePage(); });
    return page;
}

bool MainWindow::openAgentUsagePage()
{
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.id == QLatin1String("usage") && page.widget) {
            showNativePackage(page.id);
            return true;
        }
    }
    if (!m_sectionStack)
        return false;
    if (!m_agentUsageSectionPage) {
        m_agentUsageSectionPage = buildAgentUsagePage(m_sectionStack);
        m_sectionStack->addWidget(m_agentUsageSectionPage);
    }
    if (m_agentsNavButton)
        m_agentsNavButton->setChecked(true);
    m_sectionStack->setCurrentWidget(m_agentUsageSectionPage);
    scheduleNavRecord();
    updateBreadcrumb();
    return true;
}

void MainWindow::refreshAgentAccountUsageMenu(const QString &provider)
{
    const bool codex = agentIsCodexProvider(provider);
    // Usage responses arrive whether or not the menu is open, and findChild()
    // walked every widget in the window each time only to find nothing — a
    // 592 ms stall was sampled inside that recursion. The menu registers itself
    // when it is built instead.
    scheduleAgentUsagePagesRefresh();
    QMenu *menu = codex ? m_codexAccountUsageMenu : m_claudeAccountUsageMenu;
    if (!menu)
        return;

    QHash<QString, QList<AgentUsageMenuData>> usageByAccount;
    for (const AgentAccountProfile &profile : agentAccountProfiles(provider))
        usageByAccount.insert(profile.id,
                              agentAccountUsageMenuData(provider, profile.id));

    for (QAction *action : menu->actions()) {
        if (!action->objectName().startsWith(QStringLiteral("agentUsage_")))
            continue;
        const QString accountId =
            action->property("agentAccountId").toString();
        const QString usageLabel =
            action->property("agentUsageLabel").toString();
        const QList<AgentUsageMenuData> rows = usageByAccount.value(accountId);
        const auto match = std::find_if(
            rows.cbegin(), rows.cend(), [&usageLabel](const AgentUsageMenuData &row) {
                return row.label == usageLabel;
            });
        if (match == rows.cend())
            continue;
        action->setText(match->detail);
        if (auto *usageAction = qobject_cast<QWidgetAction *>(action)) {
            if (auto *row = static_cast<TokenUsageMenuRow *>(
                    usageAction->defaultWidget()))
                row->setUsageData(match->label, match->value,
                                  match->resetNote, match->usedPercent,
                                  match->resetAtMs, match->windowMs);
        }
    }
}

void MainWindow::probeClaudeAgentAccountIdentity(const QString &configDir,
                                                 QAction *accountAction,
                                                 const QString &baseLabel)
{
    if (!accountAction)
        return;
    const QString key = QDir::cleanPath(configDir);
    if (claudeCliIdentityCache().contains(key)) {
        const QString cached = claudeCliIdentityCache().value(key);
        if (!cached.isEmpty())
            accountAction->setText(baseLabel + QStringLiteral(" — ") + cached);
        return;
    }
    if (claudeCliIdentityProbes().contains(key))
        return;
    const QString claude =
        forkmesh::agentcli::executable(QStringLiteral("claude"));
    if (claude.isEmpty()) {
        claudeCliIdentityCache().insert(key, QString());
        return;
    }

    claudeCliIdentityProbes().insert(key);
    auto *process = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (!configDir.trimmed().isEmpty())
        environment.insert(QStringLiteral("CLAUDE_CONFIG_DIR"), configDir);
    process->setProcessEnvironment(environment);
    const QPointer<QAction> guardedAction(accountAction);
    auto settled = std::make_shared<bool>(false);
    auto settle = [key, process, settled] {
        if (*settled)
            return false;
        *settled = true;
        claudeCliIdentityProbes().remove(key);
        process->deleteLater();
        return true;
    };
    connect(process, &QProcess::finished, this,
            [process, key, baseLabel, guardedAction, settle](
                int exitCode, QProcess::ExitStatus exitStatus) {
                if (!settle())
                    return;
                QString identity;
                if (exitStatus == QProcess::NormalExit && exitCode == 0)
                    identity = claudeCliIdentityFromStatus(
                        process->readAllStandardOutput());
                claudeCliIdentityCache().insert(key, identity);
                if (guardedAction && !identity.isEmpty())
                    guardedAction->setText(
                        baseLabel + QStringLiteral(" — ") + identity);
            });
    connect(process, &QProcess::errorOccurred, this,
            [key, settle](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || !settle())
                    return;
                claudeCliIdentityCache().insert(key, QString());
            });
    process->start(claude,
                   {QStringLiteral("auth"), QStringLiteral("status"),
                    QStringLiteral("--json")});
    trackProcessActivity(process, QStringLiteral("agent"),
                         QStringLiteral("Read Claude Code account identity"));
    QTimer::singleShot(1500, process, [process] {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
}

void MainWindow::selectAgentAccount(const QString &provider,
                                    const QString &accountId)
{
    // Switching accounts is rare, and it is the one moment a profile written
    // outside addAgentAccount()/renameAgentAccount() (a settings edit, a restored
    // backup) has to be seen: resolve from disk rather than from the cache.
    invalidateAgentAccountProfileCache();
    const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
    if (std::none_of(profiles.cbegin(), profiles.cend(),
                     [&accountId](const AgentAccountProfile &profile) {
                         return profile.id == accountId;
                     }))
        return;

    const bool codex = agentIsCodexProvider(provider);
    const QStringList globals = codex
        ? QStringList{kCodexUsage5hPctSetting, kCodexUsageWeekPctSetting,
                      kCodexUsage5hResetSetting, kCodexUsageWeekResetSetting,
                      kCodexLimit5hStartSetting, kCodexLimitWeekStartSetting,
                      kCodexUsageWindowsSetting,
                      kCodexUsage5hExhaustedSetting,
                      kCodexUsageWeekExhaustedSetting}
        : QStringList{kClaudeUsage5hPctSetting, kClaudeUsageWeekPctSetting,
                      kClaudeUsageFablePctSetting, kClaudeUsage5hResetSetting,
                      kClaudeUsageWeekResetSetting, kClaudeUsageFableResetSetting,
                      kClaudeLimit5hStartSetting, kClaudeLimitWeekStartSetting,
                      kClaudeUsage5hExhaustedSetting,
                      kClaudeUsageWeekExhaustedSetting,
                      kClaudeUsageFableExhaustedSetting,
                      kClaudeCreditsEnabledSetting, kClaudeCreditsUsedSetting,
                      kClaudeCreditsLimitSetting, kClaudeCreditsPctSetting,
                      kClaudeCreditsCurrencySetting};
    QSettings settings;
    const QString oldId = activeAgentAccount(provider).id;
    for (const QString &global : globals) {
        if (settings.contains(global))
            settings.setValue(
                agentAccountUsageSetting(provider, oldId,
                                         agentAccountUsageField(global)),
                settings.value(global));
    }
    settings.setValue(agentActiveAccountSetting(provider), accountId);
    // activeAgentAccount() above re-primed the account-id memo with the old
    // selection; drop it so the next lookup reads the id just stored.
    invalidateAgentAccountProfileCache();
    for (const QString &global : globals) {
        const QString scoped = agentAccountUsageSetting(
            provider, accountId, agentAccountUsageField(global));
        if (settings.contains(scoped))
            settings.setValue(global, settings.value(scoped));
        else
            settings.remove(global);
    }
    restoreUsageLimitReminders();

    if (codex) {
        refreshCodexUsageRemaining();
    } else {
        m_claudeUsageLast5hPct = -1;
        m_claudeUsageLastWeekPct = -1;
        auto *chart = static_cast<TokenUsageMiniChart *>(m_navTokenUsage);
        if (chart) {
            chart->setUsage(TokenUsageMiniChart::FiveHour,
                            settings.contains(kClaudeUsage5hPctSetting)
                                ? settings.value(kClaudeUsage5hPctSetting).toInt()
                                : -1);
            chart->setUsage(TokenUsageMiniChart::Weekly,
                            settings.contains(kClaudeUsageWeekPctSetting)
                                ? settings.value(kClaudeUsageWeekPctSetting).toInt()
                                : -1);
            chart->setUsage(TokenUsageMiniChart::Fable,
                            settings.contains(kClaudeUsageFablePctSetting)
                                ? settings.value(kClaudeUsageFablePctSetting).toInt()
                                : -1);
            auto resetNote = [&](TokenUsageMiniChart::Window window,
                                 const QString &key) {
                const qint64 resetAt = settings.value(key).toLongLong();
                chart->setReset(
                    window,
                    resetAt > QDateTime::currentMSecsSinceEpoch()
                        ? humanizeRemaining(
                              resetAt - QDateTime::currentMSecsSinceEpoch())
                        : QString());
            };
            resetNote(TokenUsageMiniChart::FiveHour,
                      kClaudeUsage5hResetSetting);
            resetNote(TokenUsageMiniChart::Weekly,
                      kClaudeUsageWeekResetSetting);
            resetNote(TokenUsageMiniChart::Fable,
                      kClaudeUsageFableResetSetting);
        }
        refreshClaudeCodeUsage();
        refreshClaudeModelCombo();
    }
    refreshAgentAccountUsageMenu(provider);
    refreshAgentToolbarAccountButtons();
    refreshQuickAddAgentModelSelector();
    flashMessage(QStringLiteral("%1 account selected for new agents.")
                     .arg(codex ? QStringLiteral("Codex")
                                : QStringLiteral("Claude Code")));
}

void MainWindow::addAgentAccount(const QString &provider)
{
    const QString providerName = agentIsCodexProvider(provider)
                                     ? QStringLiteral("Codex")
                                     : QStringLiteral("Claude Code");
    bool ok = false;
    const QString label = QInputDialog::getText(
        this, QStringLiteral("Add %1 account").arg(providerName),
        QStringLiteral("Account label:"), QLineEdit::Normal,
        QStringLiteral("%1 account").arg(providerName), &ok).trimmed();
    if (!ok || label.isEmpty())
        return;
    if (!agentIsCodexProvider(provider)) {
        startClaudeAccountLogin(QString(), label);
        return;
    }
    const QString id = createAgentAccount(provider, label);
    if (id.isEmpty()) {
        flashMessage(QStringLiteral("Could not create the %1 account profile.")
                         .arg(providerName),
                     true);
        return;
    }
    selectAgentAccount(provider, id);
    launchAgentSystemTerminal(provider, QStringLiteral("login"));
}

QString MainWindow::createAgentAccount(const QString &provider, const QString &label,
                                       const QString &existingConfigDir)
{
    const QString cleanLabel = label.trimmed();
    if (cleanLabel.isEmpty())
        return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString configDir =
        existingConfigDir.isEmpty()
            ? forkmesh::paths::dataPath(QStringLiteral("agent-accounts")) +
                  QStringLiteral("/%1/%2")
                      .arg(agentAccountProviderKey(provider), id)
            : existingConfigDir;
    if (!QDir().mkpath(configDir))
        return {};
    QSettings settings;
    settings.beginGroup(agentAccountProfilesGroup(provider));
    settings.beginGroup(id);
    settings.setValue(QStringLiteral("label"), cleanLabel);
    settings.setValue(QStringLiteral("configDir"), configDir);
    settings.endGroup();
    settings.endGroup();
    invalidateAgentAccountProfileCache();
    return id;
}

void MainWindow::startClaudeAccountLogin(const QString &requestedId,
                                         const QString &label)
{
    const QString program =
        forkmesh::agentcli::executable(QStringLiteral("claude"));
    if (program.isEmpty()) {
        flashMessage(QStringLiteral("Claude Code is not installed on this device."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    const QString loginRoot = claudeAccountLoginRoot();
    QSet<QString> inUse;
    for (const AgentAccountProfile &profile :
         agentAccountProfiles(QStringLiteral("claude-code")))
        inUse.insert(QDir::cleanPath(profile.configDir));
    const QFileInfoList leftovers =
        QDir(loginRoot).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : leftovers) {
        const QString path = QDir::cleanPath(entry.absoluteFilePath());
        if (m_pendingClaudeLogins.contains(path) || inUse.contains(path))
            continue;
        m_pendingClaudeLogins.insert(path, {});
        if (finishClaudeAccountLogin(path))
            continue;
        m_pendingClaudeLogins.remove(path);
        forkmesh::safedelete::removeTree(path, loginRoot);
    }

    const QString stagingDir = QDir::cleanPath(
        loginRoot + QLatin1Char('/') +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(stagingDir)) {
        flashMessage(QStringLiteral("Could not prepare a Claude Code sign-in folder."),
                     true);
        return;
    }
    QFile::setPermissions(stagingDir, QFileDevice::ReadOwner |
                                          QFileDevice::WriteOwner |
                                          QFileDevice::ExeOwner);
    m_pendingClaudeLogins.insert(stagingDir, {requestedId, label.trimmed()});
    watchClaudeAccountLogin(stagingDir);
    const QString cwd = repoGitDir().isEmpty() ? QDir::homePath() : repoGitDir();
    if (!launchSystemTerminal(program,
                              {QStringLiteral("auth"), QStringLiteral("login")},
                              cwd,
                              {{QStringLiteral("CLAUDE_CONFIG_DIR"), stagingDir}})) {
        m_pendingClaudeLogins.remove(stagingDir);
        if (m_claudeLoginWatcher)
            m_claudeLoginWatcher->removePath(stagingDir);
        forkmesh::safedelete::removeTree(stagingDir, loginRoot);
        return;
    }
    flashMessage(QStringLiteral("Sign in to Claude Code in the terminal. The login "
                                "is filed under the account it belongs to."));
}

void MainWindow::watchClaudeAccountLogin(const QString &stagingDir)
{
    if (!m_claudeLoginWatcher) {
        m_claudeLoginWatcher = new QFileSystemWatcher(this);
        connect(m_claudeLoginWatcher, &QFileSystemWatcher::directoryChanged, this,
                [this](const QString &path) {
                    const QString dir = QDir::cleanPath(path);
                    if (!m_pendingClaudeLogins.contains(dir))
                        return;
                    const QStringList files{
                        forkmesh::agentaccounts::claudeCredentialFile(dir),
                        forkmesh::agentaccounts::claudeStateFile(dir, false)};
                    for (const QString &file : files)
                        if (QFileInfo::exists(file) &&
                            !m_claudeLoginWatcher->files().contains(file))
                            m_claudeLoginWatcher->addPath(file);
                    finishClaudeAccountLogin(dir);
                });
        connect(m_claudeLoginWatcher, &QFileSystemWatcher::fileChanged, this,
                [this](const QString &path) {
                    finishClaudeAccountLogin(
                        QDir::cleanPath(QFileInfo(path).absolutePath()));
                });
    }
    m_claudeLoginWatcher->addPath(stagingDir);
}

bool MainWindow::finishClaudeAccountLogin(const QString &stagingDir)
{
    using namespace forkmesh::agentaccounts;
    const auto pendingIt = m_pendingClaudeLogins.constFind(stagingDir);
    if (pendingIt == m_pendingClaudeLogins.cend())
        return false;
    const PendingClaudeLogin pending = *pendingIt;
    const QString provider = QStringLiteral("claude-code");
    const QString stagingState = claudeStateFile(stagingDir, false);
    const ClaudeLoginIdentity identity = readClaudeLoginIdentity(stagingState);
    if (!identity.isValid())
        return false;
    const bool fileLogin = providerAccountSignedIn(provider, stagingDir);
#if !defined(Q_OS_MACOS)
    if (!fileLogin)
        return false;
#endif

    m_pendingClaudeLogins.remove(stagingDir);
    if (m_claudeLoginWatcher) {
        const QStringList watched{stagingDir, claudeCredentialFile(stagingDir),
                                  stagingState};
        for (const QString &path : watched)
            if (m_claudeLoginWatcher->directories().contains(path) ||
                m_claudeLoginWatcher->files().contains(path))
                m_claudeLoginWatcher->removePath(path);
    }

    QSettings settings;
    QList<AccountBinding> bindings;
    for (const AgentAccountProfile &profile : agentAccountProfiles(provider))
        bindings.append(
            {profile.id,
             settings.value(agentAccountIdentitySetting(provider, profile.id))
                 .toString()});
    LoginRoute route =
        routeClaudeLogin(identity.key(), pending.requestedId, bindings);
    if (route.kind == LoginRouteKind::Adopt && !fileLogin)
        route = {};

    QString targetId = route.accountId;
    QString targetDir;
    QString error;
    if (route.kind == LoginRouteKind::New) {
        targetDir = stagingDir;
        if (fileLogin) {
            const QString accountDir =
                forkmesh::paths::dataPath(QStringLiteral("agent-accounts")) +
                QStringLiteral("/claude/") +
                QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (QDir().rename(stagingDir, accountDir))
                targetDir = accountDir;
        }
        targetId = createAgentAccount(
            provider, pending.label.isEmpty() ? identity.label() : pending.label,
            targetDir);
        if (targetId.isEmpty())
            error = QStringLiteral("could not save the account");
    } else {
        for (const AgentAccountProfile &profile : agentAccountProfiles(provider)) {
            if (profile.id != targetId)
                continue;
            targetDir = profile.configDir;
            if (fileLogin &&
                !moveClaudeLogin(stagingDir, stagingState, profile.configDir,
                                 claudeAccountStateFile(profile), &error))
                targetId.clear();
            break;
        }
    }
    if (QDir::cleanPath(targetDir) != stagingDir || targetId.isEmpty())
        forkmesh::safedelete::removeTree(stagingDir, claudeAccountLoginRoot());
    if (targetId.isEmpty()) {
        flashMessage(QStringLiteral("Signed in to Claude Code as %1, but the login "
                                    "could not be filed: %2")
                         .arg(identity.label(), error),
                     true);
        return true;
    }

    settings.setValue(agentAccountIdentitySetting(provider, targetId),
                      identity.key());
    claudeCliIdentityCache().remove(QDir::cleanPath(targetDir));
    selectAgentAccount(provider, targetId);
    QString targetLabel = identity.label();
    for (const AgentAccountProfile &profile : agentAccountProfiles(provider))
        if (profile.id == targetId)
            targetLabel = profile.label.trimmed();
    const bool requestedOther = !pending.requestedId.isEmpty() &&
                                pending.requestedId != targetId;
    flashMessage(
        requestedOther
            ? QStringLiteral("Signed in as %1, which belongs to the account %2. "
                             "Switched to %2 so each account keeps its own "
                             "sessions.")
                  .arg(identity.label(), targetLabel)
        : route.kind == LoginRouteKind::New
            ? QStringLiteral("Signed in as %1 and added the account %2.")
                  .arg(identity.label(), targetLabel)
            : QStringLiteral("Signed in as %1 on the account %2.")
                  .arg(identity.label(), targetLabel));
    return true;
}

void MainWindow::reconcileClaudeAccountLogins()
{
    using namespace forkmesh::agentaccounts;
    if (m_reconcilingClaudeAccounts)
        return;
    const QScopedValueRollback<bool> guard(m_reconcilingClaudeAccounts, true);
    const QStringList pendingDirs = m_pendingClaudeLogins.keys();
    for (const QString &stagingDir : pendingDirs)
        finishClaudeAccountLogin(stagingDir);

    const QString provider = QStringLiteral("claude-code");
    QSettings settings;
    const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
    for (const AgentAccountProfile &profile : profiles) {
        const ClaudeLoginIdentity identity =
            readClaudeLoginIdentity(claudeAccountStateFile(profile));
        if (!identity.isValid())
            continue;
        const QString setting = agentAccountIdentitySetting(provider, profile.id);
        if (settings.value(setting).toString() == identity.key())
            continue;
        settings.setValue(setting, identity.key());
        claudeCliIdentityCache().remove(QDir::cleanPath(profile.configDir));
    }
}

bool MainWindow::renameAgentAccount(const QString &provider,
                                    const QString &accountId,
                                    const QString &label)
{
    const QString cleanLabel = label.trimmed();
    if (cleanLabel.isEmpty())
        return false;
    const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
    if (std::none_of(profiles.cbegin(), profiles.cend(), [&accountId](
                         const AgentAccountProfile &profile) {
            return profile.id == accountId;
        }))
        return false;

    QSettings settings;
    if (accountId == QLatin1String("default")) {
        settings.setValue(agentAccountLabelSetting(provider, accountId),
                          cleanLabel);
    } else {
        settings.beginGroup(agentAccountProfilesGroup(provider));
        settings.beginGroup(accountId);
        settings.setValue(QStringLiteral("label"), cleanLabel);
        settings.endGroup();
        settings.endGroup();
    }
    invalidateAgentAccountProfileCache(); // the label just changed
    return true;
}

void MainWindow::editAgentAccount(const QString &provider,
                                  const QString &accountId)
{
    const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
    const auto profile = std::find_if(
        profiles.cbegin(), profiles.cend(), [&accountId](
            const AgentAccountProfile &candidate) {
            return candidate.id == accountId;
        });
    if (profile == profiles.cend())
        return;

    const QString providerName = agentIsCodexProvider(provider)
                                     ? QStringLiteral("Codex")
                                     : QStringLiteral("Claude Code");
    bool ok = false;
    const QString label = QInputDialog::getText(
        this, QStringLiteral("Edit %1 account").arg(providerName),
        QStringLiteral("Account label:"), QLineEdit::Normal, profile->label,
        &ok)
                             .trimmed();
    if (!ok || label.isEmpty() || !renameAgentAccount(provider, accountId, label))
        return;

    if (QMenu *menu = agentIsCodexProvider(provider)
                          ? m_codexAccountUsageMenu
                          : m_claudeAccountUsageMenu) {
        for (QAction *account : menu->actions()) {
            if (account->objectName() !=
                QStringLiteral("agentAccount_%1").arg(accountId))
                continue;
            const QString identity = agentCliIdentityLabel(
                provider, profile->configDir);
            QString accountLabel = label;
            if (!identity.isEmpty())
                accountLabel += QStringLiteral(" — ") + identity;
            if (!providerAccountSignedIn(provider, profile->configDir))
                accountLabel += QString::fromUtf8(" \xC2\xB7 not signed in");
            account->setText(accountLabel);
            break;
        }
    }
    refreshAgentToolbarAccountButtons(); // the bar's tooltip carries the label
    flashMessage(QStringLiteral("%1 account renamed.").arg(providerName));
}

bool MainWindow::deleteAgentAccount(const QString &provider,
                                    const QString &accountId)
{
    invalidateAgentAccountProfileCache();
    const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
    const auto profile = std::find_if(
        profiles.cbegin(), profiles.cend(), [&accountId](
            const AgentAccountProfile &candidate) {
            return candidate.id == accountId;
        });
    if (profile == profiles.cend() || profile->builtIn)
        return false;

    if (activeAgentAccount(provider).id == accountId)
        selectAgentAccount(provider, QStringLiteral("default"));

    const QString key = agentAccountProviderKey(provider);
    QSettings settings;
    settings.remove(agentAccountProfilesGroup(provider) + QLatin1Char('/') +
                    accountId);
    settings.remove(agentAccountIdentitySetting(provider, accountId));
    settings.remove(agentAccountLabelSetting(provider, accountId));
    settings.remove(
        QStringLiteral("agents/accountUsage/%1/%2").arg(key, accountId));
    invalidateAgentAccountProfileCache();

    const QString configDir = QDir::cleanPath(profile->configDir);
    QFile::remove(agentAccountCredentialPath(provider, configDir));
    const QString loginRoot = QDir::cleanPath(claudeAccountLoginRoot());
    if (configDir.startsWith(loginRoot + QLatin1Char('/')))
        QDir().rename(configDir, forkmesh::paths::dataPath(QStringLiteral("agent-accounts")) +
                                     QStringLiteral("/%1/%2")
                                         .arg(key, accountId));
    claudeCliIdentityCache().remove(configDir);

    refreshAgentToolbarAccountButtons();
    refreshQuickAddAgentModelSelector();
    return true;
}

void MainWindow::confirmDeleteAgentAccount(const QString &provider,
                                           const QString &accountId)
{
    const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
    const auto profile = std::find_if(
        profiles.cbegin(), profiles.cend(), [&accountId](
            const AgentAccountProfile &candidate) {
            return candidate.id == accountId;
        });
    if (profile == profiles.cend() || profile->builtIn)
        return;

    const QString providerName = agentIsCodexProvider(provider)
                                     ? QStringLiteral("Codex")
                                     : QStringLiteral("Claude Code");
    const QString label = profile->label.trimmed();
    QMessageBox box(QMessageBox::Warning,
                    QStringLiteral("Delete %1 account").arg(providerName),
                    QStringLiteral("Delete the %1 account \"%2\"?")
                        .arg(providerName, label),
                    QMessageBox::Cancel, this);
    box.setObjectName(QStringLiteral("agentAccountDeleteDialog"));
    box.setInformativeText(
        QStringLiteral("This desktop forgets the account and signs it out. "
                       "Agents running as it stop being able to sign in. Its "
                       "agent history stays in %1.")
            .arg(QDir::toNativeSeparators(profile->configDir)));
    QPushButton *remove =
        box.addButton(QStringLiteral("Delete account"),
                      QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != remove)
        return;
    if (!deleteAgentAccount(provider, accountId)) {
        flashMessage(QStringLiteral("Could not delete the %1 account %2.")
                         .arg(providerName, label),
                     true);
        return;
    }
    flashMessage(QStringLiteral("Deleted the %1 account %2.")
                     .arg(providerName, label));
}

void MainWindow::launchAgentSystemTerminal(const QString &provider,
                                           const QString &mode,
                                           const QString &accountId)
{
    const bool codex = agentIsCodexProvider(provider);
    const QString program = forkmesh::agentcli::executable(
        codex ? QStringLiteral("codex") : QStringLiteral("claude"));
    if (program.isEmpty()) {
        flashMessage(QStringLiteral("%1 is not installed on this device.")
                         .arg(codex ? QStringLiteral("Codex")
                                    : QStringLiteral("Claude Code")),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    QStringList commandArgs;
    if (mode == QLatin1String("logout")) {
        if (codex)
            commandArgs << QStringLiteral("logout");
        else
            commandArgs << QStringLiteral("auth") << QStringLiteral("logout");
    } else if (mode == QLatin1String("login")) {
        if (codex)
            commandArgs << QStringLiteral("login");
        else
            commandArgs << QStringLiteral("auth") << QStringLiteral("login");
    } else if (!codex) {
        commandArgs << QStringLiteral("--dangerously-skip-permissions");
    }

    if (!codex && mode == QLatin1String("login")) {
        startClaudeAccountLogin(accountId.isEmpty()
                                    ? activeAgentAccount(provider).id
                                    : accountId,
                                QString());
        return;
    }
    const QString cwd = repoGitDir().isEmpty() ? QDir::homePath() : repoGitDir();
    QMap<QString, QString> accountEnv;
    AgentAccountProfile account = activeAgentAccount(provider);
    if (!accountId.isEmpty()) {
        for (const AgentAccountProfile &profile : agentAccountProfiles(provider)) {
            if (profile.id == accountId) {
                account = profile;
                break;
            }
        }
    }
    if (!account.builtIn)
        accountEnv.insert(codex ? QStringLiteral("CODEX_HOME")
                                : QStringLiteral("CLAUDE_CONFIG_DIR"),
                          account.configDir);
    launchSystemTerminal(program, commandArgs, cwd, accountEnv);
}

// Open the host's own terminal emulator on `program args` in `cwd`, with the
// caller's environment overrides (a provider account's CLAUDE_CONFIG_DIR /
// CODEX_HOME) applied on top of a copy of ForkMesh's environment that has every
// provider API key stripped: a hand-off terminal must authenticate as the
// account the user picked, not as whatever key happened to be exported into the
// app. Split out of launchAgentSystemTerminal so the agent detail's "Pop out"
// hand-off reaches the same emulator lookup rather than repeating
// the per-platform table. Reports its own failures and returns false.
bool MainWindow::launchSystemTerminal(const QString &program,
                                      const QStringList &commandArgs,
                                      const QString &cwd,
                                      const QMap<QString, QString> &extraEnv)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("ANTHROPIC_API_KEY"));
    environment.remove(QStringLiteral("ANTHROPIC_AUTH_TOKEN"));
    environment.remove(QStringLiteral("CLAUDE_CODE_OAUTH_TOKEN"));
    environment.remove(QStringLiteral("OPENAI_API_KEY"));
    environment.remove(QStringLiteral("CODEX_API_KEY"));
    for (auto it = extraEnv.cbegin(); it != extraEnv.cend(); ++it)
        environment.insert(it.key(), it.value());

    QString terminalProgram;
    QStringList terminalArgs;
#if defined(Q_OS_WIN)
    terminalProgram = QStandardPaths::findExecutable(QStringLiteral("wt.exe"));
    if (!terminalProgram.isEmpty()) {
        terminalArgs << QStringLiteral("-d") << cwd << program << commandArgs;
    } else {
        terminalProgram = QStringLiteral("cmd.exe");
        terminalArgs << QStringLiteral("/c") << QStringLiteral("start")
                     << QString() << program << commandArgs;
    }
#elif defined(Q_OS_MACOS)
    auto shellQuote = [](QString value) {
        value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QLatin1Char('\'') + value + QLatin1Char('\'');
    };
    QString command = QStringLiteral("cd %1 && ").arg(shellQuote(cwd));
    if (!extraEnv.isEmpty()) {
        command += QStringLiteral("env ");
        for (auto it = extraEnv.cbegin(); it != extraEnv.cend(); ++it)
            command += QStringLiteral("%1=%2 ").arg(it.key(), shellQuote(it.value()));
    }
    command += shellQuote(program);
    for (const QString &argument : commandArgs)
        command += QLatin1Char(' ') + shellQuote(argument);
    QString appleCommand = command;
    appleCommand.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    appleCommand.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    terminalProgram = QStringLiteral("/usr/bin/osascript");
    terminalArgs << QStringLiteral("-e")
                 << QStringLiteral("tell application \"Terminal\" to do script \"%1\"")
                        .arg(appleCommand);
#else
    struct Candidate { const char *program; const char *separator; };
    const Candidate candidates[] = {
        {"x-terminal-emulator", "-e"}, {"gnome-terminal", "--"},
        {"konsole", "-e"}, {"xfce4-terminal", "-x"}, {"xterm", "-e"}};
    for (const Candidate &candidate : candidates) {
        terminalProgram =
            QStandardPaths::findExecutable(QString::fromLatin1(candidate.program));
        if (!terminalProgram.isEmpty()) {
            terminalArgs << QString::fromLatin1(candidate.separator)
                         << program << commandArgs;
            break;
        }
    }
#endif
    if (terminalProgram.isEmpty()) {
        flashMessage(QStringLiteral("No supported system terminal was found."), true);
        return false;
    }
    QProcess launcher;
    launcher.setProgram(terminalProgram);
    launcher.setArguments(terminalArgs);
    launcher.setWorkingDirectory(cwd);
    launcher.setProcessEnvironment(
        forkmesh::processreaper::releasedEnvironment(environment));
    if (!launcher.startDetached()) {
        flashMessage(QStringLiteral("Could not launch the system terminal."), true);
        return false;
    }
    return true;
}


void MainWindow::loadServers()
{
    // One-way migration from legacy host records: passwords used to be stored
    // inside hosts/list. Keep them only for this process lifetime, and rewrite
    // QSettings before any controller UI or fleet operation can read them.
    QSettings hostSettings;
    forkmesh::control::loadSavedHosts(
        hostSettings, kHostsSetting, &m_hostSessionPasswords);

    m_servers.clear();
    m_legacyServerUrls.clear();
    m_legacyServerRooms.clear();
    bool migratedDefaultRoom = false;
    bool migratedInstances = false;
    QSet<QString> usedInstanceIds;
    const QString json = QSettings().value(kServersArray).toString();
    const QJsonArray array = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue &value : array) {
        const QJsonObject obj = value.toObject();
        const QString url = obj.value("url").toString().trimmed();
        if (url.isEmpty())
            continue;
        m_legacyServerUrls.append(url);
        m_legacyServerRooms.append(
            obj.value("room").toString(kDefaultRoomName));
        ServerConfig server;
        server.url = canonicalServerUrl(url);
        server.room = obj.value("room").toString(kDefaultRoomName);
        server.id = obj.value("id").toString().trimmed();
        if (server.id.isEmpty() || usedInstanceIds.contains(server.id)) {
            server.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            migratedInstances = true;
        }
        usedInstanceIds.insert(server.id);
        server.name = obj.value("name").toString().trimmed().left(80);
        if (server.name.isEmpty()) {
            server.name = serverHost(server.url);
            migratedInstances = true;
        }
        server.lastAccount = AccountCapability::normalizedAccount(
            obj.value("lastAccount").toString());
        migratedDefaultRoom |=
            forkmesh::mainnode::migrateSavedDefaultRoom(
                &server.url, &server.room);
        m_servers.append(server);
    }

    if (m_servers.isEmpty()) {
        const QString savedUrl =
            QSettings().value(kServerUrlSetting).toString().trimmed();
        ServerConfig server;
        const QString savedRoom =
            QSettings().value(kRoomNameSetting, kDefaultRoomName).toString();
        m_legacyServerUrls.append(savedUrl);
        m_legacyServerRooms.append(savedRoom);
        server.url = (savedUrl.isEmpty() || savedUrl == kLocalServerUrl)
                         ? kDefaultServerUrl
                         : savedUrl;
        server.room = savedRoom;
        server.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        server.name = serverHost(server.url);
        server.lastAccount = AccountCapability::normalizedAccount(
            QSettings().value(kAccountNameSetting).toString());
        migratedInstances = true;
        migratedDefaultRoom |=
            forkmesh::mainnode::migrateSavedDefaultRoom(
                &server.url, &server.room);
        m_servers.append(server);
    }

    m_activeServer = QSettings().value(kActiveServerSetting, 0).toInt();
    if (m_activeServer < 0 || m_activeServer >= m_servers.size())
        m_activeServer = 0;
    const QString legacyActiveAccount = AccountCapability::normalizedAccount(
        QSettings().value(kAccountNameSetting).toString());
    if (isValidNodeName(legacyActiveAccount) &&
        m_servers.at(m_activeServer).lastAccount.isEmpty()) {
        m_servers[m_activeServer].lastAccount = legacyActiveAccount;
        migratedInstances = true;
    }
    QString legacySingleUrl =
        QSettings().value(kServerUrlSetting).toString().trimmed();
    QString legacySingleRoom =
        QSettings().value(kRoomNameSetting, kDefaultRoomName).toString();
    migratedDefaultRoom |=
        forkmesh::mainnode::migrateSavedDefaultRoom(
            &legacySingleUrl, &legacySingleRoom);
    if (migratedDefaultRoom || migratedInstances)
        saveServers();
    else
        recordProjectInstances();
}

void MainWindow::saveServers()
{
    if (m_activeServer < 0 || m_activeServer >= m_servers.size())
        m_activeServer = qBound(0, m_activeServer, qMax(0, m_servers.size() - 1));

    QJsonArray array;
    for (const ServerConfig &server : std::as_const(m_servers)) {
        array.append(QJsonObject{{"id", server.id},
                                 {"name", server.name},
                                 {"url", server.url},
                                 {"room", server.room},
                                 {"lastAccount", server.lastAccount}});
    }
    QSettings settings;
    settings.setValue(kServersArray,
                      QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    settings.setValue(kActiveServerSetting, m_activeServer);

    if (!m_servers.isEmpty()) {
        const ServerConfig &active = m_servers.at(m_activeServer);
        settings.setValue(kServerUrlSetting, active.url);
        settings.setValue(kRoomNameSetting, active.room);
        forgetConfiguredRelayHost();
    }
    recordProjectInstances();
}

QString MainWindow::activeServerInstanceId() const
{
    if (m_activeServer < 0 || m_activeServer >= m_servers.size())
        return {};
    return m_servers.at(m_activeServer).id.trimmed();
}

QString MainWindow::activeServerInstanceUrl() const
{
    if (m_activeServer < 0 || m_activeServer >= m_servers.size())
        return {};
    return canonicalServerUrl(m_servers.at(m_activeServer).url);
}

bool MainWindow::activeServerMatches(const QString &instanceId,
                                     const QString &instanceUrl) const
{
    const QString activeId = activeServerInstanceId();
    const QString activeUrl = activeServerInstanceUrl();
    return !activeId.isEmpty() && !instanceId.trimmed().isEmpty() &&
           activeId == instanceId.trimmed() &&
           !activeUrl.isEmpty() &&
           activeUrl == canonicalServerUrl(instanceUrl);
}

void MainWindow::loadActiveServerIntoEdits()
{
    if (m_activeServer < 0 || m_activeServer >= m_servers.size())
        return;
    const ServerConfig &active = m_servers.at(m_activeServer);
    if (m_serverUrlEdit)
        m_serverUrlEdit->setText(serverHostDisplay(active.url)); // show host only
    if (m_roomNameEdit)
        m_roomNameEdit->setText(active.room);
    QTimer::singleShot(0, this, [this] { refreshInstanceTheme(); });
}

void MainWindow::persistEditsToActiveServer()
{
    if (m_activeServer < 0 || m_activeServer >= m_servers.size())
        return;
    ServerConfig &active = m_servers[m_activeServer];
    const QString editedUrl = canonicalServerUrl(m_serverUrlEdit->text());
    if (forkmesh::projects::instanceKey(editedUrl) != forkmesh::projects::instanceKey(active.url) &&
        refuseInstanceOfAnotherProject(editedUrl)) {
        m_serverUrlEdit->setText(serverHostDisplay(active.url));
        return;
    }
    active.url = editedUrl; // host -> full URL
    active.room = m_roomNameEdit->text().trimmed();
    if (isValidNodeName(active.lastAccount))
        rememberUserAccount(active.lastAccount, active.url);
    saveServers();
}

QPixmap MainWindow::faviconFor(const ServerConfig &server) const
{
    const QString host = serverHost(server.url);
    if (m_faviconCache.contains(host))
        return roundedRectPixmap(m_faviconCache.value(host), 36, 9);
    return letterFavicon(host); // already drawn as a rounded rect
}

void MainWindow::switchToServer(int index)
{
    if (index < 0 || index >= m_servers.size())
        return;
    const bool live = m_backend != nullptr;
    if (index == m_activeServer && live) {
        showSection(0); // already connected here: just jump to its Home
        return;
    }

    if (live)
        persistEditsToActiveServer(); // capture any edits to the current server
    if (m_activeServer >= 0 && m_activeServer < m_servers.size() &&
        hasActiveAccountSession()) {
        ServerConfig &previous = m_servers[m_activeServer];
        const QString account =
            AccountCapability::normalizedAccount(m_accountName);
        const bool tokenlessLocalCapability =
            m_accountSessionToken.trimmed().isEmpty() &&
            m_accountSessionInstanceUrl.trimmed().isEmpty();
        const QUrl previousProfile = accountApiUrlForInstance(
            previous.url, QStringLiteral("profile"));
        const bool sessionBelongsToPrevious =
            tokenlessLocalCapability ||
            !accountSessionTokenForUrl(previousProfile).isEmpty();
        if (isValidNodeName(account) && sessionBelongsToPrevious) {
            previous.lastAccount = account;
            cacheActiveAccountSession(previous.url);
            rememberUserAccount(account, previous.url);
        }
    }
    // Persist the old instance under its own stable id before changing the
    // selector. The new session clears these structures during startup.
    saveChatHistory();
    m_activeServer = index;
    m_networkRepoMirrorsCache.clear();
    m_networkRepoMirrorsFetchedMs.clear();
    m_privateCatalogAccessIds.clear();
    m_repositoryCalendarMonths.clear();
    m_catalogMirrorsCache = {};
    m_catalogMirrorsSource.clear();
    m_catalogMirrorsFetchedMs = 0;
    m_mirrorReachabilityCache.clear();
    m_mirrorPendingCache.clear();
    m_releaseDownloadsCache.clear();
    m_directMirrorDiscoveryInFlight.clear();
    loadActiveServerIntoEdits();
    const QString account = m_servers.at(index).lastAccount;
    isolateAccountForServerSwitch(account);
    if (isValidNodeName(account))
        restoreCachedAccountSession(account, m_servers.at(index).url);
    // The gateway configuration names the repositories served to one relay.
    // Rewrite it for the relay just selected before the new session starts,
    // so nothing added under the previous relay is served to this one.
    QString gatewayError;
    (void)rebuildDirectMirrorGatewayConfiguration(&gatewayError);
    saveServers();
    updateBreadcrumb();
    startSession(); // tears down the old backend and connects to the new server
    scheduleAgentQueuePump(); // resume only work stamped for this instance
}

void MainWindow::promptAddServer()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Add mainnode server");
    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText(QStringLiteral("Work, personal, staging…"));
    auto *urlEdit = new QLineEdit(&dialog);
    urlEdit->setPlaceholderText(kDefaultServerUrl);
    auto *roomEdit = new QLineEdit(kDefaultRoomName, &dialog);
    roomEdit->setReadOnly(true);

    auto *form = new QFormLayout;
    form->addRow("Instance name", nameEdit);
    form->addRow("Server URL", urlEdit);
    form->addRow("Room", roomEdit);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *dialogLayout = new QVBoxLayout(&dialog);
    dialogLayout->addLayout(form);
    dialogLayout->addWidget(buttons);
    dialog.resize(460, 200);
    if (dialog.exec() != QDialog::Accepted)
        return;

    ServerConfig server;
    server.url = urlEdit->text().trimmed();
    if (server.url.isEmpty())
        server.url = kDefaultServerUrl;
    const QString legacyUrl = server.url;
    server.url = canonicalServerUrl(server.url);
    if (refuseInstanceOfAnotherProject(server.url))
        return;
    server.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    server.name = nameEdit->text().trimmed().left(80);
    if (server.name.isEmpty())
        server.name = serverHost(server.url);
    server.room = kDefaultRoomName;
    m_servers.append(server);
    m_legacyServerUrls.append(legacyUrl);
    m_legacyServerRooms.append(server.room);
    const int newIndex = m_servers.size() - 1;
    saveServers();
    updateBreadcrumb();
    fetchFavicon(newIndex);
    switchToServer(newIndex);
}

void MainWindow::renameServer(int index)
{
    if (index < 0 || index >= m_servers.size())
        return;
    const ServerConfig &server = m_servers.at(index);
    bool accepted = false;
    const QString name = QInputDialog::getText(
                             this, QStringLiteral("Rename relay instance"),
                             QStringLiteral("Instance name:"),
                             QLineEdit::Normal,
                             server.name.isEmpty() ? serverHost(server.url)
                                                   : server.name,
                             &accepted)
                             .trimmed()
                             .left(80);
    if (!accepted || name.isEmpty())
        return;
    m_servers[index].name = name;
    saveServers();
    updateBreadcrumb();
    refreshRelayNodes();
}

void MainWindow::removeServer(int index)
{
    if (index < 0 || index >= m_servers.size() || m_servers.size() <= 1)
        return;
    if (QMessageBox::question(
            this, "Remove server",
            QStringLiteral("Stop tracking %1?").arg(serverHost(m_servers.at(index).url))) !=
        QMessageBox::Yes)
        return;

    const bool removingActive = (index == m_activeServer);
    const ServerConfig removed = m_servers.at(index);
    if (removingActive)
        saveChatHistory();
    forgetServerInstance(removed);
    m_servers.removeAt(index);
    if (index < m_legacyServerUrls.size())
        m_legacyServerUrls.removeAt(index);
    if (index < m_legacyServerRooms.size())
        m_legacyServerRooms.removeAt(index);
    if (m_activeServer > index)
        --m_activeServer;
    if (m_activeServer >= m_servers.size())
        m_activeServer = m_servers.size() - 1;
    saveServers();
    loadActiveServerIntoEdits();
    updateBreadcrumb();
    if (removingActive) {
        const QString account = m_servers.at(m_activeServer).lastAccount;
        isolateAccountForServerSwitch(account);
        if (isValidNodeName(account))
            restoreCachedAccountSession(account,
                                        m_servers.at(m_activeServer).url);
        startSession(); // reconnect to whichever server is now active
        scheduleAgentQueuePump();
    }
}


void MainWindow::loadCachedFavicons()
{
    for (const ServerConfig &server : std::as_const(m_servers)) {
        const QString host = serverHost(server.url);
        const QString path = faviconCachePath(host);
        QPixmap pix;
        if (QFileInfo::exists(path) && pix.load(path) && !pix.isNull()) {
            m_faviconCache.insert(host, pix);
            m_logFaviconPixmaps.remove(host); // re-draw from the real icon
        }
    }
}

void MainWindow::fetchFavicon(int index)
{
    if (index < 0 || index >= m_servers.size())
        return;
    fetchFaviconFromUrl(serverHost(m_servers.at(index).url),
                        faviconUrl(m_servers.at(index).url));
}

void MainWindow::fetchFaviconForHost(const QString &host)
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(host);
    url.setPath(QStringLiteral("/favicon.ico"));
    fetchFaviconFromUrl(host, url);
}

void MainWindow::fetchFaviconFromUrl(const QString &host, const QUrl &url)
{
    if (host.isEmpty() || m_faviconCache.contains(host) ||
        m_faviconFetching.contains(host) || m_faviconMissing.contains(host))
        return;

    // Hosts are harvested from log lines, so reserved special-use names the
    // app itself synthesises (forkmesh.internal, used for Worker cache keys
    // and tests) arrive here whenever a CLOUD line mentions one.  They can
    // never answer, and m_faviconMissing is in-memory only, so each launch
    // re-ran the same doomed lookup.  Record it as missing without a request.
    if (hostCannotServePublicFavicon(host)) {
        m_faviconMissing.insert(host);
        return;
    }

    const QPixmap builtin = builtinFavicon(host);
    if (!builtin.isNull()) {
        m_faviconCache.insert(host, builtin);
        refreshLogFavicon(host);
        return;
    }

    QPixmap disk;
    const QString cached = faviconCachePath(host);
    if (QFileInfo::exists(cached) && disk.load(cached) && !disk.isNull()) {
        m_faviconCache.insert(host, disk);
        refreshLogFavicon(host);
        return;
    }

    if (!url.isValid() || url.host().isEmpty()) {
        m_faviconMissing.insert(host);
        return;
    }
    if (!m_networkAccess)
        return;

    m_faviconFetching.insert(host);
    QNetworkRequest request(url);
    forkmesh::markFaviconProbe(request);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, host, url] {
        reply->deleteLater();
        m_faviconFetching.remove(host);
        // ForkMesh relays publish their real favicon as a static asset rather
        // than at the browser-conventional root path.  Try that asset after a
        // standard favicon request fails, retaining support for other relays.
        const auto tryRelayAsset = [this, host, url] {
            if (url.path() != faviconRootPath())
                return false;
            fetchFaviconFromUrl(host, faviconAssetUrl(url));
            return true;
        };
        if (reply->error() != QNetworkReply::NoError) {
            if (tryRelayAsset())
                return;
            m_faviconMissing.insert(host);
            return;
        }
        QPixmap pix;
        if (!pix.loadFromData(reply->readAll()) || pix.isNull()) {
            if (tryRelayAsset())
                return;
            m_faviconMissing.insert(host);
            return;
        }
        if (pix.width() > 64)
            pix = pix.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_faviconCache.insert(host, pix);
        QDir().mkpath(faviconCacheDir());
        pix.save(faviconCachePath(host), "PNG");
        updateBreadcrumb();
        refreshLogFavicon(host);
    });
}


QWidget *MainWindow::buildChatPage()
{
    auto *page = new QWidget;
    auto *shell = new QWidget;

    m_sectionStack = new forkmesh::ui::SectionViewport;
    connect(m_sectionStack, &QStackedWidget::currentChanged, this,
            [this](int index) {
                updateQuickAddEnterTarget();
                updateRepoActivityRail();
                renderPageNotices();
                noteGeniePage();
                updatePackageActionsButton();
                refreshDirectFocus();
                if (index == 4)
                    setLogOverlayExpanded(false);
                else
                    positionGlobalFooterOverlays();
            });
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("buildChatPage: home section"));
        m_sectionStack->addWidget(buildHomeSection()); // 0 Home (nodes + repos + detail)
    }
    logStartup(QStringLiteral("  buildChatPage: home section built"));
    auto addDeferredSection = [this] {
        auto *placeholder = new QWidget;
        placeholder->setProperty("forkmeshDeferredSection", true);
        m_sectionStack->addWidget(placeholder);
    };
    addDeferredSection();                              // 1 Settings
    // Chat owns a large transcript/composer tree. The Home shell is the first
    // page, so production leaves Chat behind the same package-neutral loading
    // placeholder as every other secondary section and builds it on click.
#ifdef FORKMESH_WINDOW_TESTS
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("buildChatPage: chat section"));
        m_sectionStack->addWidget(buildChatSection()); // 2 Chat
    }
#else
    addDeferredSection();                              // 2 Chat
#endif
    for (int index = 3; index <= 8; ++index)
        addDeferredSection();
    m_sectionStack->addWidget(new QWidget);              // 9 retired Firewall redirect
    for (int index = 10; index <= kSessionsSectionIndex; ++index)
        addDeferredSection();
    logStartup(QStringLiteral("  buildChatPage: secondary sections deferred"));

    auto *content = new QWidget;
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    contentLayout->addWidget(buildPageNoticeArea());
    contentLayout->addWidget(m_sectionStack, 1);

    QWidget *header = nullptr;
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("buildChatPage: breadcrumb header"));
        header = buildBreadcrumb();
    }
    auto *layout = new QVBoxLayout(shell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("buildChatPage: wallet notices"));
        layout->addWidget(buildSolanaNotice());
        layout->addWidget(buildWalletVerifyNotice());
    }
    layout->addWidget(buildSecurityAlarmBanner());
    content->setMinimumHeight(0);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);

    auto *bodyLayout = new QVBoxLayout;
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(content, 1); // content absorbs window growth
    m_globalOverlayHost = content;
    content->installEventFilter(this);
#ifndef FORKMESH_WINDOW_TESTS
    logStartup(QStringLiteral(
        "  buildChatPage: global agent composer deferred until after first paint"));
#endif
    layout->addLayout(bodyLayout, 1);

    auto *rail = new QWidget;
    rail->setObjectName(QStringLiteral("appNavigationRailContent"));
    rail->setMinimumWidth(railColumnWidth());
    auto *railOuterLayout = new QVBoxLayout(rail);
    railOuterLayout->setContentsMargins(0, 0, 0, 0);
    railOuterLayout->setSpacing(0);
    railOuterLayout->addWidget(buildRailSortButton(), 0, Qt::AlignLeft);
    m_appNavigationRailLayout = new QVBoxLayout;
    m_appNavigationRailLayout->setContentsMargins(0, 0, 0, 4);
    m_appNavigationRailLayout->setSpacing(1);
    railOuterLayout->addLayout(m_appNavigationRailLayout, 1);
    for (QPushButton *button :
         {m_homeGridNavButton, m_sessionsNavButton, m_agentsNavButton,
          m_reposNavButton,
          m_marketNavButton, m_ledgerNavButton, m_chatButton,
          m_controlNodeNavButton,
          m_networkNavButton, m_usersNavButton, m_teamNavButton,
          m_filesNavButton, m_statsNavButton, m_componentHealthNavButton,
          m_memoryNavButton})
        m_appNavigationRailLayout->addWidget(button, 0, Qt::AlignLeft);
    syncNativePackagePages();
    m_repoViewButton->setParent(header);
    m_repoViewButton->hide();

    for (QPushButton *button :
         {m_tourNavButton, m_settingsNavButton, m_navScreenshotButton,
          m_notesNavButton, m_calendarNavButton, m_tasksNavButton,
          m_notificationButton})
        m_appNavigationRailLayout->addWidget(button, 0, Qt::AlignLeft);
    applyRailOrder(); // packs every item against the top, stretch at the end
    syncPackageSurfaces();

    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("buildChatPage: notification button"));
        updateNotificationButton();
    }

    auto *railScroll = new QScrollArea;
    railScroll->setObjectName(QStringLiteral("appNavigationRail"));
    railScroll->setWidget(rail);
    railScroll->setWidgetResizable(true);
    railScroll->setFrameShape(QFrame::NoFrame);
    railScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    railScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    railScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    railScroll->setFixedWidth(railWidth());
    railScroll->setMinimumHeight(0);
    railScroll->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    m_appNavigationRail = railScroll;
    applyRailCaptionPreference();

    auto *lower = new QWidget;
    auto *lowerLayout = new QHBoxLayout(lower);
    lowerLayout->setContentsMargins(0, 0, 0, 0);
    lowerLayout->setSpacing(0);
    lowerLayout->addWidget(railScroll);
    lowerLayout->addWidget(forkmesh::ui::NextAccess::railResizeHandle(*this));
    lowerLayout->addWidget(shell, 1);
    lowerLayout->addWidget(buildPackageRail());
    lowerLayout->addWidget(buildTeammateRail());

    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(header);
    root->addWidget(lower, 1);
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("buildChatPage: status bar"));
        root->addWidget(buildStatusBar());
    }
#ifdef FORKMESH_WINDOW_TESTS
    // Keep the completed test fixture synchronous, but construct it in the
    // same order as production: the status strip first and the deferred
    // composer/activity model second. This catches regressions where a
    // late-created footer statistic never gets attached to the existing bar.
    initializeDeferredGlobalLogDock();
#endif
    return page;
}

void MainWindow::initializeDeferredGlobalLogDock()
{
    if (m_footerDock || !m_globalOverlayHost)
        return;
    forkmesh::StartupTraceStep step(
        QStringLiteral("post-paint: build global agent composer"));
    QWidget *logDock = buildNetworkLogDock();
    if (!logDock)
        return;
    logDock->setParent(m_globalOverlayHost);
    loadPromptOverlayPlacement();
    positionGlobalFooterOverlays();
    setupGenieSpider();
}

QWidget *MainWindow::buildStatusBar()
{
    auto *statusArea = new QWidget;
    statusArea->setObjectName(QStringLiteral("appStatusArea"));
    m_appStatusArea = statusArea;
    auto *statusAreaLayout = new QVBoxLayout(statusArea);
    statusAreaLayout->setContentsMargins(0, 0, 0, 0);
    statusAreaLayout->setSpacing(0);

    auto *bar = new QWidget;
    bar->setObjectName("appStatusBar");

    m_branchButton = new QPushButton("main");
    m_branchButton->setObjectName("ghostButton");
    m_branchButton->setCursor(Qt::PointingHandCursor);
    m_branchButton->setToolTip("Switch branch");
    setOcticon(m_branchButton, "git-branch", 12);

    m_footerWorktreeInfo = new QLabel;
    m_footerWorktreeInfo->setObjectName("footerWorktreeInfo");
    m_footerWorktreeInfo->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_footerWorktreeInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_footerWorktreeInfo->hide();

    m_footerGitIdentity = new QLabel;
    m_footerGitIdentity->setObjectName("footerGitIdentity");
    m_footerGitIdentity->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_footerGitIdentity->setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    m_footerGitIdentity->setOpenExternalLinks(false);
    connect(m_footerGitIdentity, &QLabel::linkActivated, this,
            [this](const QString &link) {
                if (link == QLatin1String("set-git-identity"))
                    setRepoGitIdentityFromForkMesh();
            });
    m_footerGitIdentity->setToolTip(
        "Git author identity configured for the repository you're viewing");

    m_footerCommitInfo = new QLabel;
    m_footerCommitInfo->setObjectName("footerCommitInfo");
    m_footerCommitInfo->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_footerCommitInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_footerCommitInfo->setToolTip("Commit the browsed branch points at");

    const QString appPath =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString appFolder =
        QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    m_statusAppPath = new QLabel;
    m_statusAppPath->setObjectName("statusAppPath");
    m_statusAppPath->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_statusAppPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusAppPath->setText(m_statusAppPath->fontMetrics().elidedText(
        QStringLiteral("Folder: %1").arg(appFolder), Qt::ElideMiddle, 420));
    m_statusAppPath->setToolTip(
        QStringLiteral("Running app: %1\nWorking directory: %2")
            .arg(appPath, QDir::toNativeSeparators(QDir::currentPath())));

    for (QLabel *label : {m_footerWorktreeInfo, m_footerGitIdentity,
                          m_footerCommitInfo, m_statusAppPath}) {
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    // Unlike the branch metadata, the running-build location must remain
    // visible at the right edge. An Ignored horizontal policy allowed the two
    // stretches to compress this label to zero pixels even on wide windows.
    m_statusAppPath->setMinimumWidth(72);
    m_statusAppPath->setMaximumWidth(420);
    m_statusAppPath->setSizePolicy(QSizePolicy::Preferred,
                                   QSizePolicy::Preferred);
    m_footerCommitInfo->setMinimumWidth(96);
    m_footerCommitInfo->setMaximumWidth(700);
    m_footerCommitInfo->setSizePolicy(QSizePolicy::Preferred,
                                      QSizePolicy::Preferred);
    m_footerGitIdentity->setMinimumWidth(48);
    m_footerGitIdentity->setMaximumWidth(460);
    m_footerGitIdentity->setSizePolicy(QSizePolicy::Preferred,
                                       QSizePolicy::Preferred);

    auto *versionButton = new QPushButton(QStringLiteral("v" FORKMESH_VERSION));
    m_statusVersionButton = versionButton;
    versionButton->setObjectName(QStringLiteral("statusVersionButton"));
    versionButton->setCheckable(true);
    versionButton->setCursor(Qt::PointingHandCursor);
    versionButton->setAccessibleName(QStringLiteral("Toggle debug bar"));
    versionButton->setToolTip(QStringLiteral("Show debug activity and resource use"));

    // Offline mode lives next to the version because that is the one strip
    // that is on screen on every page. The label names the mode and its
    // state (issue #695: a bare "Offline" read the same in both switch
    // positions); updateOfflineModeControls() reserves the wider text's
    // width so the switch flipping does not reflow the bar.
    auto *offlineLabel = new QLabel(QStringLiteral("Offline mode: Off"));
    m_statusOfflineLabel = offlineLabel;
    offlineLabel->setObjectName(QStringLiteral("statusOfflineLabel"));
    offlineLabel->setAccessibleName(QStringLiteral("Offline mode"));
    offlineLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *offlineToggle = new ToggleSwitch;
    m_statusOfflineToggle = offlineToggle;
    offlineToggle->setObjectName(QStringLiteral("statusOfflineToggle"));
    offlineToggle->setSwitchSize(QSize(28, 14));
    offlineToggle->setAccessibleName(QStringLiteral("Toggle offline mode"));
    connect(offlineToggle, &QAbstractButton::clicked, this,
            [this](bool checked) { setOfflineMode(checked); });

    m_statusBackgroundHost = new QWidget;
    m_statusBackgroundHost->setObjectName("statusBackgroundTasks");
    m_statusBackgroundLayout = new QHBoxLayout(m_statusBackgroundHost);
    m_statusBackgroundLayout->setContentsMargins(0, 0, 0, 0);
    m_statusBackgroundLayout->setSpacing(5);
    m_statusBackgroundLabel = new QLabel(QStringLiteral("Background:"));
    m_statusBackgroundLabel->setObjectName("statusBackgroundLabel");
    m_statusBackgroundLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_statusBackgroundLabel->setToolTip(
        QStringLiteral("Background work in progress"));
    m_statusBackgroundLabel->hide();
    m_statusBackgroundLayout->addWidget(m_statusBackgroundLabel);

    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(10, 0, 10, 0);
    row->setSpacing(10);
    row->addWidget(m_branchButton);
    row->addWidget(m_footerWorktreeInfo);
    row->addWidget(m_footerCommitInfo);
    row->addWidget(m_footerGitIdentity);
    row->addStretch(1);
    row->addWidget(m_statusBackgroundHost);
    row->addStretch(1);
    row->addWidget(m_statusAppPath);
    row->addWidget(versionButton);
    row->addWidget(offlineLabel);
    row->addWidget(offlineToggle);

    updateOfflineModeControls();

    const int rowHeight = qMax(20, bar->fontMetrics().height() + 6);
    bar->setFixedHeight(rowHeight);
    m_branchButton->setMaximumHeight(rowHeight - 2);
    statusAreaLayout->addWidget(bar);

    auto *debugBar = new QFrame;
    m_debugBar = debugBar;
    debugBar->setObjectName(QStringLiteral("debugBar"));
    auto *debugBarLayout = new QHBoxLayout(debugBar);
    debugBarLayout->setContentsMargins(0, 0, 0, 0);
    debugBarLayout->setSpacing(0);

    auto *debugScroll = new QScrollArea;
    debugScroll->setObjectName(QStringLiteral("debugBarScroll"));
    debugScroll->setFrameShape(QFrame::NoFrame);
    debugScroll->setWidgetResizable(true);
    debugScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    debugScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    debugScroll->setFixedHeight(51);

    auto *debugContent = new QWidget;
    debugContent->setObjectName(QStringLiteral("debugBarContent"));
    auto *debugRow = new QHBoxLayout(debugContent);
    m_debugActivityLayout = debugRow;
    debugRow->setContentsMargins(8, 2, 8, 2);
    debugRow->setSpacing(8);
    debugRow->setSizeConstraint(QLayout::SetMinimumSize);
    if (m_resourceChart) {
        m_resourceChart->setObjectName(QStringLiteral("debugResourceChart"));
        m_resourceChart->setFixedSize(40, 40);
        debugRow->addWidget(m_resourceChart, 0, Qt::AlignVCenter);
    }
    auto *uptimeTile = new ResourceSparkline(QStringLiteral("UP"), debugContent,
                                             40, 2);
    m_debugUptimeTile = uptimeTile;
    uptimeTile->setObjectName(QStringLiteral("debugUptimeTile"));
    uptimeTile->setAccessibleName(QStringLiteral("ForkMesh session uptime"));
    uptimeTile->setToolTip(
        QStringLiteral("How long this ForkMesh window has been running"));
    debugRow->addWidget(uptimeTile, 0, Qt::AlignVCenter);

    auto *networkActivityTile =
        new ResourceSparkline(QStringLiteral("NET"), debugContent, 40, 60);
    m_debugNetworkActivityTile = networkActivityTile;
    networkActivityTile->setObjectName(
        QStringLiteral("debugNetworkActivityTile"));
    networkActivityTile->setAccessibleName(
        QStringLiteral("Live network request activity"));
    networkActivityTile->setToolTip(
        QStringLiteral("Network request attempts per second for this session"));
    debugRow->addWidget(networkActivityTile, 0, Qt::AlignVCenter);

    auto *logActivityTile =
        new ResourceSparkline(QStringLiteral("LOG"), debugContent, 40, 60);
    m_debugLogActivityTile = logActivityTile;
    logActivityTile->setObjectName(QStringLiteral("debugLogActivityTile"));
    logActivityTile->setAccessibleName(QStringLiteral("Live log activity"));
    logActivityTile->setToolTip(
        QStringLiteral("Log entries per second for this session"));
    debugRow->addWidget(logActivityTile, 0, Qt::AlignVCenter);

    const struct {
        const char *name;
        const char *label;
        const char *accessible;
        bool healthColors;
    } qtTiles[] = {
        {"qtDiagnosticLoopTile", "LOOP", "Qt event-loop latency", true},
        {"qtDiagnosticEventTile", "EVT", "Qt event delivery rate", false},
        {"qtDiagnosticPaintTile", "PAINT", "Qt paint and layout activity", false},
        {"qtDiagnosticUiTile", "UI", "Qt widgets and windows", false},
        {"qtDiagnosticPoolTile", "POOL", "Qt thread pool and timers", true},
    };
    for (const auto &definition : qtTiles) {
        auto *tile = new ResourceSparkline(
            QString::fromLatin1(definition.label), debugContent, 40, 60);
        tile->setObjectName(QString::fromLatin1(definition.name));
        tile->setAccessibleName(QString::fromLatin1(definition.accessible));
        tile->setHealthColorsEnabled(definition.healthColors);
        tile->setToolTip(QStringLiteral("Open Qt runtime diagnostics"));
        tile->onClicked = [this] { showQtDiagnosticsDialog(); };
        m_qtDiagnosticTiles.append(tile);
        debugRow->addWidget(tile, 0, Qt::AlignVCenter);
    }
    auto *separator = new QFrame;
    separator->setObjectName(QStringLiteral("debugBarSeparator"));
    separator->setFrameShape(QFrame::VLine);
    separator->setFixedHeight(32);
    debugRow->addWidget(separator, 0, Qt::AlignVCenter);
    auto *allLogsButton = new ActivityRailButton(
        QStringLiteral("list-unordered"), QStringLiteral("All"));
    m_debugLogAllButton = allLogsButton;
    allLogsButton->setObjectName(QStringLiteral("debugLogAllButton"));
    allLogsButton->setCheckable(false);
    allLogsButton->setCompact(true);
    allLogsButton->setBadgeCount(m_debugAllLogCount);
    allLogsButton->setToolTip(
        QStringLiteral("Open every debug log category on the full Log page"));
    connect(allLogsButton, &QPushButton::clicked, this,
            &MainWindow::openFullLogForAll);
    debugRow->addWidget(allLogsButton, 0, Qt::AlignVCenter);
    debugRow->addStretch(1);
    attachDebugActivityLights();
    debugScroll->setWidget(debugContent);
    debugBarLayout->addWidget(debugScroll, 1);

    auto *debugTools = new QWidget;
    debugTools->setObjectName(QStringLiteral("debugBarTools"));
    auto *debugToolsRow = new QHBoxLayout(debugTools);
    debugToolsRow->setContentsMargins(4, 2, 8, 2);
    debugToolsRow->setSpacing(2);
    auto *toolsSeparator = new QFrame;
    toolsSeparator->setObjectName(QStringLiteral("debugBarSeparator"));
    toolsSeparator->setFrameShape(QFrame::VLine);
    toolsSeparator->setFixedHeight(32);
    debugToolsRow->addWidget(toolsSeparator, 0, Qt::AlignVCenter);


    auto *logTailButton = new ActivityRailButton(QStringLiteral("list-unordered"),
                                                 QStringLiteral("Log"));
    m_debugLogTailButton = logTailButton;
    logTailButton->setObjectName("topNavButton");
    logTailButton->setCursor(Qt::PointingHandCursor);
    logTailButton->setToolTip(
        QString::fromUtf8("Expand the window with the last %1 log lines, live "
                       "\xE2\x80\x94 this app and the Worker. Drag it out "
                       "of the window to open the full Log in its own window")
            .arg(kDebugLogTailLines));
    connect(logTailButton, &QPushButton::toggled, this,
            [this](bool on) { setDebugLogTailVisible(on); });
    attachPageDragOut(logTailButton, [this] { popOutLogPage(); });
    logTailButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(logTailButton, &QWidget::customContextMenuRequested, this,
            [this, logTailButton](const QPoint &pos) {
                auto *menu = new QMenu(this);
                menu->setAttribute(Qt::WA_DeleteOnClose);
                menu->addAction(QStringLiteral("Open Log in new window"), this,
                                [this] { popOutLogPage(); });
                menu->popup(logTailButton->mapToGlobal(pos));
            });

    auto *copyDiagnosticsButton = new ActivityRailButton(QStringLiteral("copy"),
                                                         QStringLiteral("Report"));
    m_debugCopyDiagnosticsButton = copyDiagnosticsButton;
    copyDiagnosticsButton->setObjectName("topNavButton");
    copyDiagnosticsButton->setAccessibleName(QStringLiteral("Copy diagnostic report"));
    copyDiagnosticsButton->setCheckable(false);
    copyDiagnosticsButton->setCursor(Qt::PointingHandCursor);
    copyDiagnosticsButton->setToolTip(QString::fromUtf8(
        "Copy diagnostic report \xE2\x80\x94 errors, stalls, crashes, resource "
        "findings and fix-it instructions to paste into an agent. Each copy "
        "compares itself with the last one, so it shows what got fixed and "
        "what is new. Right-click to send it straight to a new agent"));
    connect(copyDiagnosticsButton, &QPushButton::clicked, this,
            &MainWindow::copyDiagnosticsReport);
    copyDiagnosticsButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(copyDiagnosticsButton, &QWidget::customContextMenuRequested, this,
            [this, copyDiagnosticsButton](const QPoint &pos) {
                auto *menu = new QMenu(this);
                menu->setAttribute(Qt::WA_DeleteOnClose);
                menu->addAction(QStringLiteral("Copy diagnostic report"), this,
                                [this] { copyDiagnosticsReport(); });
                menu->addAction(QStringLiteral("Send report to a new agent"), this,
                                [this] { sendDiagnosticsReportToAgent(); });
                menu->addSeparator();
                menu->addAction(QStringLiteral("Start comparisons over"), this,
                                [this] { resetDiagnosticsReportBaseline(); });
                menu->popup(copyDiagnosticsButton->mapToGlobal(pos));
            });

    auto *sendDiagnosticsButton = new ActivityRailButton(
        QStringLiteral("rocket"), QStringLiteral("Send diagnostic"));
    sendDiagnosticsButton->setObjectName(QStringLiteral("sendDiagnosticsButton"));
    sendDiagnosticsButton->setAccessibleName(QStringLiteral("Send diagnostic"));
    sendDiagnosticsButton->setCheckable(false);
    sendDiagnosticsButton->setCursor(Qt::PointingHandCursor);
    sendDiagnosticsButton->setToolTip(QStringLiteral(
        "Start a desktop agent with a diagnostic snapshot to fix errors, "
        "investigate crashes and stalls, and optimize resource usage"));
    connect(sendDiagnosticsButton, &QPushButton::clicked, this,
            &MainWindow::sendDiagnosticsReportToAgent);

    for (QPushButton *tool : {m_navRebuildButton, m_navResizeButton,
                              static_cast<QPushButton *>(copyDiagnosticsButton),
                              static_cast<QPushButton *>(sendDiagnosticsButton),
                              static_cast<QPushButton *>(logTailButton)})
        if (tool)
            debugToolsRow->addWidget(tool, 0, Qt::AlignVCenter);
    debugBarLayout->addWidget(debugTools, 0);
    debugBar->hide();
    statusAreaLayout->addWidget(debugBar);

    // The tail is the footer log in miniature: same widget kind, same line
    // HTML and the same viewport filter, so the prompt icon, the background
    // task mark, the favicon and the source anchor all work here too.
    m_debugLogTail = new QTextEdit;
    m_debugLogTail->setObjectName(QStringLiteral("debugLogTail"));
    m_debugLogTail->setReadOnly(true);
    m_debugLogTail->setFrameShape(QFrame::NoFrame);
    m_debugLogTail->setLineWrapMode(QTextEdit::NoWrap);
    m_debugLogTail->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_debugLogTail->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_debugLogTail->document()->setDocumentMargin(0);
    m_debugLogTail->document()->setUndoRedoEnabled(false);
    m_debugLogTail->setToolTip(
        QString::fromUtf8("Live log tail \xE2\x80\x94 the newest %1 lines from "
                       "this app and from the Worker (CLOUD). Click a line to "
                       "open the full Log at it, or its + to add it to the "
                       "prompt")
            .arg(kDebugLogTailLines));
    QFont tailFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    tailFont.setPixelSize(11);
    m_debugLogTail->setFont(tailFont);
    m_debugLogTail->viewport()->setCursor(Qt::PointingHandCursor);
    m_debugLogTail->viewport()->installEventFilter(this);
    styleFooterUpdateLog();
    // The rendered lines carry 11px icons, so a line is as tall as the taller
    // of the font and the icon; the padding above matches the stylesheet.
    m_debugLogTailHeight =
        kDebugLogTailLines * qMax(QFontMetrics(tailFont).lineSpacing(), 13) + 8;
    m_debugLogTail->setFixedHeight(m_debugLogTailHeight);
    m_debugLogTail->hide();
    statusAreaLayout->addWidget(m_debugLogTail);

    connect(versionButton, &QPushButton::toggled, this,
            [this, versionButton](bool expanded) {
                if (m_debugBar)
                    m_debugBar->setVisible(expanded);
                if (!expanded && m_debugLogTailButton)
                    m_debugLogTailButton->setChecked(false);
                if (m_qtDiagnostics)
                    m_qtDiagnostics->setDetailedEnabled(expanded);
                versionButton->setToolTip(
                    expanded ? QStringLiteral("Hide debug activity and resource use")
                             : QStringLiteral("Show debug activity and resource use"));
            });

    QTimer::singleShot(0, this, [this] { applyDebugBarStartupPreference(); });
    QTimer::singleShot(kCloudLogMonitorStartupDelayMs, this,
                       [this] { startCloudLogMonitorIfConfigured(); });
    return statusArea;
}

void MainWindow::applyDebugBarStartupPreference()
{
    if (m_debugBarStartupApplied || !m_statusVersionButton)
        return;
    QSettings settings;
    const bool configured = settings.contains(kShowDebugBarOnStartupSetting);
    const bool show = configured
                          ? settings.value(kShowDebugBarOnStartupSetting).toBool()
                          : m_isAdmin;
    if (!show) {
        m_debugBarStartupApplied = configured;
        return;
    }
    m_debugBarStartupApplied = true;
    if (!configured && m_debugBarStartupCheck) {
        const QSignalBlocker blocker(*m_debugBarStartupCheck);
        m_debugBarStartupCheck->setChecked(true);
    }
    m_statusVersionButton->setChecked(true);
}

void MainWindow::seedDebugLogTail()
{
    if (!m_debugLogTail)
        return;
    QString seedHtml;
    QStringList seedLines;
    for (int i = qMax(0, m_networkLog.size() - kDebugLogTailLines);
         i < m_networkLog.size(); ++i) {
        const QString clean = m_networkLog.at(i).trimmed();
        if (clean.isEmpty())
            continue;
        seedHtml += QStringLiteral("<div>%1</div>")
                        .arg(footerLogLineHtml(clean, m_debugLogTail));
        seedLines << clean;
    }
    m_debugLogTail->setHtml(seedHtml);
    int seedIndex = 0;
    for (QTextBlock b = m_debugLogTail->document()->firstBlock();
         b.isValid() && seedIndex < seedLines.size(); b = b.next(), ++seedIndex)
        b.setUserData(new FooterLogLineData(seedLines.at(seedIndex)));
}

void MainWindow::setDebugLogTailVisible(bool visible)
{
    if (!m_debugLogTail || m_debugLogTailShown == visible)
        return;
    m_debugLogTailShown = visible;
    const int strip = m_debugLogTailHeight;
    if (visible)
        seedDebugLogTail();
    m_debugLogTail->setVisible(visible);
    if (m_debugLogTailButton && m_debugLogTailButton->isChecked() != visible)
        m_debugLogTailButton->setChecked(visible);
    if (visible)
        if (QScrollBar *bar = m_debugLogTail->verticalScrollBar())
            bar->setValue(bar->maximum());
    if (!isMaximized() && !isFullScreen())
        resize(width(), qMax(minimumHeight(),
                             height() + (visible ? strip : -strip)));
}


QWidget *MainWindow::buildNetworkLogDock()
{
    auto *dock = new QWidget;
    m_footerDock = dock;
    dock->setObjectName("logDock");
    const bool next = Theme::isNext();

    m_issueQuickAdd = new QPlainTextEdit;
    m_issueQuickAdd->setObjectName("issueQuickAdd");
    m_issueQuickAdd->setPlaceholderText("enter prompt");
    m_issueQuickAdd->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_issueQuickAdd->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_issueQuickAdd->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_issueQuickAdd->document()->setDocumentMargin(2);
    // With interface sounds on, typing a prompt ticks softly (UiSounds.h).
    m_issueQuickAdd->setProperty(forkmesh::sonify::kTypingSoundProperty, true);
    (new PromptSpellCheckHighlighter(m_issueQuickAdd->document()))
        ->attachTo(m_issueQuickAdd);
    const int kQuickAddRowH = m_issueQuickAdd->fontMetrics().lineSpacing();
    m_issueQuickAdd->setMinimumHeight(kQuickAddRowH * 4 + 8 + 4);
    m_issueQuickAdd->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const int kQuickAddMaxChars = 16000;
    auto *quickAddInputHost = new QWidget;
    auto *quickAddInputLayout = new QGridLayout(quickAddInputHost);
    quickAddInputLayout->setContentsMargins(0, 0, 0, 0);
    quickAddInputLayout->setSpacing(0);
    quickAddInputLayout->addWidget(m_issueQuickAdd, 0, 0);

    m_issueQuickAdd->installEventFilter(this);
    m_quickAddHistory = QSettings().value(kQuickAddHistorySetting).toStringList();
    forkmesh::logStartupTrace(QStringLiteral("  logDock: composer built"));

    m_quickAddCharCount = new QLabel;
    m_quickAddCharCount->setObjectName("quickAddCharCount");
    m_quickAddCharCount->setToolTip("Characters remaining in the quick-add title");
    m_quickAddCharCount->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_quickAddCharCount->setFixedWidth(
        m_quickAddCharCount->fontMetrics().horizontalAdvance(
            QString::number(kQuickAddMaxChars)) +
        6);
    auto updateQuickAddCharCount = [this, kQuickAddMaxChars]() {
        const int remaining =
            kQuickAddMaxChars - m_issueQuickAdd->toPlainText().length();
        m_quickAddCharCount->setText(QString::number(remaining));
        m_quickAddCharCount->setStyleSheet(Theme::remapSheet(QStringLiteral(
            "QLabel#quickAddCharCount{color:%1;font-size:11px;}")
                .arg(remaining <= 20 ? QStringLiteral("#d29922")
                                     : QStringLiteral("#8b949e"))));
    };
    connect(m_issueQuickAdd, &QPlainTextEdit::textChanged, this,
            [this, updateQuickAddCharCount, kQuickAddMaxChars] {
                const QString text = m_issueQuickAdd->toPlainText();
                if (text.length() > kQuickAddMaxChars) {
                    const QSignalBlocker block(m_issueQuickAdd);
                    m_issueQuickAdd->setPlainText(text.left(kQuickAddMaxChars));
                    m_issueQuickAdd->moveCursor(QTextCursor::End);
                }
                updateQuickAddCharCount();
                if (!m_quickAddHistoryNavigating)
                    m_quickAddHistoryIndex = -1;
            });
    updateQuickAddCharCount();

    // Keep what is being typed on disk so a restart or crash mid-prompt puts it
    // back. Edits coalesce for a moment before the write, and quitting flushes
    // whatever is still pending. Paging through history with Up/Down is not
    // typing: the draft on disk stays the text the user left to go browsing.
    m_quickAddDraftSaveTimer = new QTimer(this);
    m_quickAddDraftSaveTimer->setSingleShot(true);
    m_quickAddDraftSaveTimer->setInterval(250);
    connect(m_quickAddDraftSaveTimer, &QTimer::timeout, this,
            &MainWindow::saveQuickAddDraft);
    m_issueQuickAdd->setPlainText(PromptDraftStore::load());
    m_issueQuickAdd->moveCursor(QTextCursor::End);
    connect(m_issueQuickAdd, &QPlainTextEdit::textChanged, this, [this] {
        if (m_quickAddHistoryNavigating)
            return;
        m_quickAddPendingDraft = m_issueQuickAdd->toPlainText();
        m_quickAddDraftDirty = true;
        m_quickAddDraftSaveTimer->start();
    });

    m_quickAddAgentProvider = new FullPopupComboBox;
    m_quickAddAgentProvider->setObjectName("quickAddAgentSelector");
    m_quickAddAgentProvider->addItem(QStringLiteral("Manual"),
                                     QStringLiteral("manual"));
    m_quickAddAgentProvider->addItem(QStringLiteral("ActivityPub"),
                                     kActivityPubPromptProvider);
    m_quickAddAgentProvider->addItem(QStringLiteral("Note"),
                                     kNotePromptProvider);
    m_quickAddAgentProvider->addItem(QStringLiteral("Task"),
                                     kTaskPromptProvider);
    m_quickAddAgentProvider->addItem(QStringLiteral("Codex"), kCodexProvider);
    m_quickAddAgentProvider->addItem(QStringLiteral("OpenAI"),
                                     QStringLiteral("openai"));
    m_quickAddAgentProvider->addItem(QStringLiteral("Claude API"),
                                     QStringLiteral("claude-api"));
    m_quickAddAgentProvider->addItem(QStringLiteral("CC"),
                                     QStringLiteral("claude-code"));
    m_quickAddAgentProvider->addItem(QStringLiteral("CF AI"),
                                     kCloudflareAiProvider);
    m_quickAddAgentProvider->addItem(QStringLiteral("Local"),
                                     kLocalLlamaProvider);
    selectQuickAddAgentProvider(m_quickAddAgentProvider);
    m_quickAddAgentProvider->setToolTip(
        "What picks this prompt up: CC (Claude Code) or Codex run the CLI agents, "
        "OpenAI/Claude API run the headless API agents, CF AI runs a headless "
        "agent on a relay Cloudflare Workers AI model, Local uses the installed "
        "Llama server, Manual files an issue, Note saves a note on this "
        "computer, Task assigns an organization task to the member you pick, "
        "and ActivityPub publishes a post to this repository's fediverse "
        "followers.");
    m_quickAddAgentProvider->setMaxVisibleItems(30);
    m_quickAddAgentProvider->view()->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_quickAddClaudeModel = new FullPopupComboBox;
    m_quickAddClaudeModel->setObjectName("quickAddModelSelector");
    m_quickAddClaudeModel->setMaxVisibleItems(30);
    m_quickAddAgentModelSelector = new FullPopupComboBox;
    m_quickAddAgentModelSelector->setObjectName("quickAddAgentModelSelector");
    m_quickAddAgentModelSelector->setIconSize(QSize(22, 22));
    m_quickAddAgentModelSelector->view()->setIconSize(QSize(26, 26));
    m_quickAddAgentModelSelector->setMaxVisibleItems(30);
    m_quickAddAgentModelSelector->setItemDelegate(
        new AgentChoiceDescriptionDelegate(m_quickAddAgentModelSelector));
    m_quickAddAgentModelSelector->setToolTip(
        "Choose the agent and model that will handle this prompt.");
    m_quickAddTaskAssignee = new FullPopupComboBox;
    m_quickAddTaskAssignee->setObjectName("quickAddTaskAssignee");
    m_quickAddTaskAssignee->setIconSize(QSize(18, 18));
    m_quickAddTaskAssignee->view()->setIconSize(QSize(22, 22));
    m_quickAddTaskAssignee->setMaxVisibleItems(30);
    m_quickAddTaskAssignee->setVisible(false);
    connect(m_quickAddTaskAssignee,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index < 0)
                    return;
                const QString assignment =
                    m_quickAddTaskAssignee->itemData(index).toString();
                QSettings().setValue(kQuickAddTaskAssigneeSetting, assignment);
                const QString who = m_quickAddTaskAssignee->itemText(index);
                const QString tip =
                    assignment.startsWith(QLatin1String("user:"))
                        ? QStringLiteral("Submitting assigns the task to %1.")
                              .arg(who)
                        : QStringLiteral("Submitting adds the task to General "
                                         "with nobody assigned.");
                m_quickAddTaskAssignee->setToolTip(
                    tip + QStringLiteral(" Click to pick someone else in your "
                                         "organization."));
                m_quickAddTaskAssignee->setAccessibleName(
                    QStringLiteral("Task assignee: %1").arg(who));
                updateQuickAddEnterTarget();
            });
    auto refreshQuickAddModelPicker = [this]() {
        if (!m_quickAddAgentProvider || !m_quickAddClaudeModel)
            return;
        const QString provider = m_quickAddAgentProvider->currentData().toString();
        const QSignalBlocker block(m_quickAddClaudeModel);
        if (provider == QLatin1String("claude-code")) {
            populateClaudeModelCombo(m_quickAddClaudeModel);
            m_quickAddClaudeModel->setProperty("claudeModelCombo", true);
            m_quickAddClaudeModel->setToolTip(
                "Claude model the Claude Code agent runs as (passed to the CLI as --model).");
            selectModelComboValue(
                m_quickAddClaudeModel,
                QSettings().value(kClaudeCodeModelSetting).toString().trimmed());
            applyLiveClaudeModelsToCombos();
        } else if (agentIsCodexProvider(provider)) {
            populateCodexModelCombo(m_quickAddClaudeModel);
            m_quickAddClaudeModel->setProperty("claudeModelCombo", false);
            m_quickAddClaudeModel->setToolTip(
                "Codex model passed to the Codex CLI.");
            selectModelComboValue(
                m_quickAddClaudeModel,
                codexChatGptModelId(
                    QSettings().value(kCodexModelSetting).toString().trimmed()));
        } else if (agentIsCloudflareAiProvider(provider)) {
            populateCloudflareAiModelCombo(m_quickAddClaudeModel);
            m_quickAddClaudeModel->setToolTip(
                "Cloudflare Workers AI model the relay runs this agent on.");
            selectModelComboValue(
                m_quickAddClaudeModel,
                QSettings().value(kCloudflareAiModelSetting).toString().trimmed());
        } else if (agentIsLocalLlamaProvider(provider)) {
            fillAgentFixModelCombo(m_quickAddClaudeModel, provider);
            m_quickAddClaudeModel->setToolTip(
                "Installed local model. Auto asks Ollama for the first installed model.");
            selectModelComboValue(
                m_quickAddClaudeModel,
                QSettings().value(kLocalLlamaModelSetting).toString().trimmed());
        } else {
            m_quickAddClaudeModel->setProperty("claudeModelCombo", false);
        }
    };
    m_quickAddClaudeModel->view()->installEventFilter(this);
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("logDock: populate the model pickers"));
        refreshQuickAddModelPicker();
        refreshCloudflareAiModels();
    }
    auto persistQuickAddModel = [this]() {
        refreshQuickAddSpeedSelector();
        if (!m_quickAddAgentProvider || !m_quickAddClaudeModel)
            return;
        const QString provider = m_quickAddAgentProvider->currentData().toString();
        const QString model = selectedModelComboValue(m_quickAddClaudeModel);
        if (provider == QLatin1String("claude-code")) {
            QSettings().setValue(kClaudeCodeModelSetting, model);
        } else if (agentIsCodexProvider(provider)) {
            const QString safeModel = codexChatGptModelId(model);
            QSettings().setValue(kCodexModelSetting, safeModel);
            if (m_codexModelEdit)
                m_codexModelEdit->setText(safeModel);
        } else if (agentIsCloudflareAiProvider(provider)) {
            QSettings().setValue(kCloudflareAiModelSetting, model);
        } else if (agentIsLocalLlamaProvider(provider)) {
            QSettings().setValue(kLocalLlamaModelSetting, model);
        }
        refreshQuickAddAgentModelSelector();
        scheduleTeammateRailRefresh();
    };
    connect(m_quickAddClaudeModel, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [persistQuickAddModel](int) { persistQuickAddModel(); });
    connect(m_quickAddClaudeModel, &QComboBox::currentTextChanged, this,
            [persistQuickAddModel](const QString &) { persistQuickAddModel(); });
    // Permission/sandbox mode for both structured CLI integrations. Claude maps
    // this to its skip-permissions switch; Codex app-server maps every option to
    // a distinct approval policy and sandbox, including interactive requests.
    m_quickAddModeSelector = new IconWordFullPopupComboBox;
    m_quickAddModeSelector->setObjectName("quickAddModeSelector");
    m_quickAddModeSelector->addItem(composerModeIcon(7), kClaudeAutoModeLabel, true);
    m_quickAddModeSelector->addItem(composerModeIcon(8), kAgentAskModeLabel, false);
    m_quickAddModeSelector->addItem(composerModeIcon(9), QStringLiteral("Plan"), false);
    m_quickAddModeSelector->addItem(composerModeIcon(10), QStringLiteral("Edit"), false);
    // The four labels name the mode but not what it permits — the closed control
    // carries the one word beside its icon, and the permission each one grants is
    // spelled out beside the label once the popup is open. The item text itself
    // stays as-is: it is what kAgentModeSetting persists and what the launch
    // paths compare against.
    {
        const QStringList permissions = {
            QStringLiteral("run without asking"),
            QStringLiteral("confirm every change"),
            QStringLiteral("read-only, propose a plan"),
            QStringLiteral("edit files, ask before commands"),
        };
        for (int i = 0; i < permissions.size() && i < m_quickAddModeSelector->count();
             ++i) {
            m_quickAddModeSelector->setItemData(i, permissions.at(i),
                                                kAgentChoiceDescriptionRole);
            m_quickAddModeSelector->setItemData(
                i,
                QStringLiteral("%1 — %2")
                    .arg(m_quickAddModeSelector->itemText(i), permissions.at(i)),
                Qt::ToolTipRole);
        }
    }
    m_quickAddModeSelector->view()->setIconSize(QSize(26, 26));
    m_quickAddModeSelector->setMaxVisibleItems(30);
    m_quickAddModeSelector->setToolTip(
        "How much freedom the agent has to make changes without asking first.");
    {
        const QString savedMode =
            QSettings().value(kAgentModeSetting).toString().trimmed();
        int idx = savedMode.isEmpty() ? -1 : m_quickAddModeSelector->findText(savedMode);
        if (idx < 0)
            idx = m_quickAddModeSelector->findData(
                QSettings().value(kClaudeAutoModeSetting, true).toBool());
        m_quickAddModeSelector->setCurrentIndex(
            idx >= 0 ? idx : m_quickAddModeSelector->count() - 1);
    }
    connect(m_quickAddModeSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                QSettings().setValue(kClaudeAutoModeSetting,
                                     m_quickAddModeSelector->currentData().toBool());
                QSettings().setValue(kAgentModeSetting,
                                     m_quickAddModeSelector->currentText());
                const QString description =
                    QStringLiteral("Permission mode: %1")
                        .arg(m_quickAddModeSelector->currentText());
                m_quickAddModeSelector->setAccessibleName(description);
                m_quickAddModeSelector->setToolTip(
                    description + QStringLiteral(". Click to choose a mode."));
            });
    {
        const QString description =
            QStringLiteral("Permission mode: %1")
                .arg(m_quickAddModeSelector->currentText());
        m_quickAddModeSelector->setAccessibleName(description);
        m_quickAddModeSelector->setToolTip(
            description + QStringLiteral(". Click to choose a mode."));
    }
    m_quickAddSpeedSelector = new IconOnlyFullPopupComboBox;
    m_quickAddSpeedSelector->setObjectName("quickAddSpeedSelector");
    m_quickAddSpeedSelector->setMaxVisibleItems(30);
    m_quickAddSpeedSelector->setToolTip(
        "Speed: how hard the model thinks about each turn (the CLI's reasoning "
        "effort). Higher is slower and more thorough.");
    connect(m_quickAddSpeedSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                const QString level =
                    m_quickAddSpeedSelector->currentData().toString();
                if (level.isEmpty())
                    return;
                QSettings().setValue(kClaudeEffortSetting, level);
                if (m_slashActionsPopup && m_slashActionsPopup->isVisible())
                    populateSlashActionsList();
            });
    refreshQuickAddSpeedSelector();
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("logDock: claude effort levels"));
        refreshClaudeEffortLevels();
    }
    m_quickAddCreatePr = new QCheckBox("Create PR");
    m_quickAddCreatePr->setToolTip(
        "When quick-add assigns an agent, create a pull request from its patch.");
    m_quickAddCreatePr->setVisible(false);
    m_quickAddImageButton = new QPushButton;
    m_quickAddImageButton->setObjectName("ghostButton");
    m_quickAddImageButton->setCursor(Qt::PointingHandCursor);
    setComposerIconTone(m_quickAddImageButton);
    setOcticon(m_quickAddImageButton, "paperclip", 16);
    connect(m_quickAddImageButton, &QPushButton::clicked, this,
            &MainWindow::attachQuickAddMedia);
    m_quickAddAttachStrip = new QWidget;
    m_quickAddAttachStrip->setObjectName("quickAddAttachStrip");
    auto *attachStripRow = new QHBoxLayout(m_quickAddAttachStrip);
    attachStripRow->setContentsMargins(0, 0, 0, 0);
    attachStripRow->setSpacing(4);
    m_quickAddAttachStrip->setVisible(false);
    updateQuickAddImageButton();

    m_quickAddMicButton = new QPushButton;
    m_quickAddMicButton->setObjectName("ghostButton");
    m_quickAddMicButton->setCursor(Qt::PointingHandCursor);
    setComposerIconTone(m_quickAddMicButton);
    setOcticon(m_quickAddMicButton, "mic", 16);
    connect(m_quickAddMicButton, &QPushButton::pressed, this,
            &MainWindow::startVoiceCapture);
    connect(m_quickAddMicButton, &QPushButton::released, this,
            &MainWindow::stopVoiceCapture);
    m_voiceLevelMeter = new QProgressBar;
    m_voiceLevelMeter->setObjectName("voiceLevelMeter");
    m_voiceLevelMeter->setRange(0, 100);
    m_voiceLevelMeter->setValue(0);
    m_voiceLevelMeter->setTextVisible(false);
    m_voiceLevelMeter->setFixedSize(48, 12);
    m_voiceLevelMeter->setToolTip("Live microphone input level");
    m_voiceLevelMeter->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "QProgressBar#voiceLevelMeter{border:1px solid #30363d;border-radius:3px;"
        "background:#0d1117;}"
        "QProgressBar#voiceLevelMeter::chunk{background:%1;border-radius:2px;}")
        .arg(Theme::nextOrHex(Theme::Role::Accent, QStringLiteral("#3fb950")))));
    m_voiceLevelMeter->setVisible(false);
    m_quickAddCreatePr->setChecked(false);
    m_quickAddCreatePr->setEnabled(true);
    m_quickAddAgentProvider->setEnabled(true);
    auto syncQuickAddAgentControls = [this]() {
        const QString provider = m_quickAddAgentProvider->currentData().toString();
        const bool claudeCode = provider == QLatin1String("claude-code");
        const bool codex = agentIsCodexProvider(provider);
        m_quickAddAgentProvider->setVisible(false);
        m_quickAddClaudeModel->setVisible(false);
        m_quickAddAgentModelSelector->setVisible(true);
        const bool task = provider == kTaskPromptProvider;
        if (task) {
            refreshQuickAddTaskAssignees();
            if (organizationTaskMemberNames().isEmpty() &&
                m_organizationTasksStartupPayload.isEmpty())
                refreshOrganizationTaskBadge();
        }
        m_quickAddTaskAssignee->setVisible(task);
        m_quickAddModeSelector->setVisible(claudeCode || codex);
        if (m_quickAddSpeedSelector) {
            m_quickAddSpeedSelector->setVisible(claudeCode || codex);
            refreshQuickAddSpeedSelector();
        }
        if (m_navCodexUsage)
            m_navCodexUsage->setVisible(codex);
        if (m_navTokenUsage)
            m_navTokenUsage->setVisible(claudeCode);
    };
    connect(m_quickAddAgentProvider, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, syncQuickAddAgentControls, refreshQuickAddModelPicker](int) {
                QSettings().setValue(
                    kQuickAddAgentProviderSetting,
                    m_quickAddAgentProvider->currentData().toString());
                refreshQuickAddModelPicker();
                syncQuickAddAgentControls();
                refreshQuickAddAgentModelSelector();
                updateQuickAddEnterTarget();
                scheduleTeammateRailRefresh();
            });
    connect(m_quickAddAgentModelSelector,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index < 0 || !m_quickAddAgentProvider ||
                    !m_quickAddClaudeModel)
                    return;
                const QString provider =
                    m_quickAddAgentModelSelector->itemData(index).toString();
                if (provider == kEditComposerModelsRow) {
                    refreshQuickAddAgentModelSelector();
                    QTimer::singleShot(0, this,
                                       [this] { openComposerModelsPage(); });
                    return;
                }
                const QString model = m_quickAddAgentModelSelector
                                          ->itemData(index, Qt::UserRole + 1)
                                          .toString();
                const QSignalBlocker selectorBlock(m_quickAddAgentModelSelector);
                const int providerIndex =
                    m_quickAddAgentProvider->findData(provider);
                if (providerIndex >= 0 &&
                    providerIndex != m_quickAddAgentProvider->currentIndex())
                    m_quickAddAgentProvider->setCurrentIndex(providerIndex);
                if (!model.isEmpty())
                    selectModelComboValue(m_quickAddClaudeModel, model);
                refreshQuickAddSpeedSelector();
                refreshQuickAddAgentModelSelector();
            });
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("logDock: agent model selector"));
        refreshQuickAddAgentModelSelector();
    }

    m_quickAddSlashButton = new QPushButton(QStringLiteral("/"));
    m_quickAddSlashButton->setObjectName("quickAddSlashButton");
    m_quickAddSlashButton->setCursor(Qt::PointingHandCursor);
    m_quickAddSlashButton->setFixedSize(22, 22);
    m_quickAddSlashButton->setToolTip(
        "Agent commands and quick actions");
    connect(m_quickAddSlashButton, &QPushButton::clicked, this,
            &MainWindow::openQuickAddSlashActions);

    m_quickAddSendButton = new QPushButton(QStringLiteral("new"));
    m_quickAddSendButton->setObjectName("quickAddSendIcon");
    m_quickAddSendButton->setCursor(Qt::PointingHandCursor);
    m_quickAddSendButton->setProperty("composerGlyph", QStringLiteral("plus"));
    tintComposerActionButton(m_quickAddSendButton);
    m_quickAddSendButton->setFixedWidth(kComposerActionWidth);
    m_quickAddSendButton->setMinimumHeight(28);
    m_quickAddSendButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_quickAddSendButton, &QPushButton::clicked, this,
            &MainWindow::quickAddIssue);

    m_quickAddSendToAgentButton = new QPushButton(QStringLiteral("add"));
    m_quickAddSendToAgentButton->setObjectName("quickAddSendIcon");
    m_quickAddSendToAgentButton->setCursor(Qt::PointingHandCursor);
    m_quickAddSendToAgentButton->setProperty("composerGlyph",
                                             QStringLiteral("paper-airplane"));
    tintComposerActionButton(m_quickAddSendToAgentButton);
    m_quickAddSendToAgentButton->setFixedWidth(kComposerActionWidth);
    m_quickAddSendToAgentButton->setMinimumHeight(28);
    m_quickAddSendToAgentButton->setSizePolicy(QSizePolicy::Fixed,
                                               QSizePolicy::Fixed);
    connect(m_quickAddSendToAgentButton, &QPushButton::clicked, this, [this] {
        if (!m_issueQuickAdd)
            return;
        const QString typed = m_issueQuickAdd->toPlainText().trimmed();
        if (sendPromptToChatRoom(typed))
            return;
        // A cloud run open above takes the message through the relay. Its node
        // cannot read a file on this disk, so attachments stay in the composer.
        if (const int cloud = quickAddCloudFollowUpSessionId()) {
            if (!m_quickAddImages.isEmpty()) {
                flashMessage(QStringLiteral(
                                 "A cloud agent cannot open files attached "
                                 "here — remove the attachments and send the "
                                 "text on its own."),
                             true);
                return;
            }
            if (!typed.isEmpty()) {
                recordQuickAddHistory(typed);
                floatPromptIntoTranscript(typed, {});
                m_issueQuickAdd->clear();
            }
            // An empty send is "continue", as it is for a local session.
            sendCloudAgentFollowUp(cloud, typed);
            return;
        }
        // What is on screen names the conversation — the transcript open above,
        // the agent whose branch diff is being read, or the agent attached to
        // the selected pull request. Only when none is showing does the last
        // selection stand in.
        const int onScreen = quickAddFollowUpAgentSessionId();
        const int target = onScreen >= 0 ? onScreen : m_selectedAgentSessionId;
        if (target < 0) {
            logSystem(QString::fromUtf8(
                "No agent open above to send that to \xE2\x80\x94 open one first."));
            return;
        }
        if (typed.isEmpty() && m_quickAddImages.isEmpty()) {
            applyComposerSelectionToAgentSession(target);
            continueAgentSessionInteractive(target);
            return;
        }
        QString prompt = typed;
        for (const QString &attachment : m_quickAddImages) {
            if (!prompt.isEmpty() && !prompt.endsWith(QLatin1Char('\n')))
                prompt += QLatin1Char('\n');
            prompt += promptAttachmentLine(attachment);
        }
        if (!typed.isEmpty())
            recordQuickAddHistory(typed);
        floatPromptIntoTranscript(prompt, m_quickAddImages);
        m_issueQuickAdd->clear();
        clearQuickAddImages();
        applyComposerSelectionToAgentSession(target);
        if (target == m_selectedAgentSessionId && m_agentTranscript)
            m_agentTranscript->jumpToBottom();
        sendPromptToAgentSession(target, prompt);
    });

    m_quickAddGenieButton = new QPushButton(QStringLiteral("task"));
    m_quickAddGenieButton->setObjectName("quickAddGenieButton");
    m_quickAddGenieButton->setCursor(Qt::PointingHandCursor);
    m_quickAddGenieButton->setProperty("composerGlyph", QStringLiteral("checklist"));
    tintComposerActionButton(m_quickAddGenieButton);
    m_quickAddGenieButton->setFixedWidth(kComposerActionWidth);
    m_quickAddGenieButton->setMinimumHeight(next ? kComposerControlPx : 24);
    m_quickAddGenieButton->setSizePolicy(QSizePolicy::Fixed,
                                         QSizePolicy::Fixed);
    m_quickAddGenieButton->setToolTip(
        QString::fromUtf8("Task \xE2\x80\x94 add this prompt to the organization's "
                          "general task list."));
    connect(m_quickAddGenieButton, &QPushButton::clicked, this,
            &MainWindow::createQuickAddOrganizationTask);

    m_issueQuickAdd->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *sendColumn = new QVBoxLayout;
    sendColumn->setContentsMargins(next ? 4 : 0, 0, next ? 8 : 0, next ? 4 : 0);
    sendColumn->setSpacing(next ? 4 : 2);
    sendColumn->addStretch(1);
    sendColumn->addWidget(m_quickAddGenieButton, 0);
    sendColumn->addWidget(m_quickAddSendToAgentButton, 0);
    sendColumn->addWidget(m_quickAddSendButton, 0);
    updateQuickAddEnterTarget();

    auto *agentBox = new QWidget;
    agentBox->setObjectName("quickAddAgentBox");
    auto *agentBoxRow = new QHBoxLayout(agentBox);
    agentBoxRow->setContentsMargins(next ? 0 : 6, next ? 0 : 1, next ? 0 : 4,
                                    next ? 0 : 1);
    agentBoxRow->setSpacing(next ? 4 : 2);
    agentBoxRow->addWidget(m_quickAddAgentModelSelector);
    agentBoxRow->addWidget(m_quickAddTaskAssignee);
    agentBoxRow->addWidget(m_quickAddAgentProvider);
    agentBoxRow->addWidget(m_quickAddClaudeModel);
    agentBoxRow->addWidget(m_quickAddModeSelector);
    agentBoxRow->addWidget(m_quickAddSpeedSelector);

    if (next) {
        for (QPushButton *button :
             {m_quickAddImageButton, m_quickAddMicButton, m_quickAddSlashButton}) {
            button->setMinimumWidth(kComposerControlPx);
            button->setFixedHeight(kComposerControlPx);
        }
        m_quickAddSlashButton->setFixedWidth(kComposerControlPx);
        for (QPushButton *button : {m_quickAddSendButton, m_quickAddSendToAgentButton,
                                    m_quickAddGenieButton})
            button->setFixedHeight(kComposerControlPx);
        m_quickAddAgentModelSelector->setIconSize(QSize(0, 0));
        m_quickAddAgentModelSelector->view()->setIconSize(
            QSize(kComposerIconPx, kComposerIconPx));
        m_quickAddModeSelector->setIconSize(QSize(kComposerIconPx, kComposerIconPx));
        m_quickAddModeSelector->view()->setIconSize(
            QSize(kComposerIconPx, kComposerIconPx));
        m_quickAddSpeedSelector->setIconSize(QSize(kComposerIconPx, kComposerIconPx));
        m_quickAddSpeedSelector->view()->setIconSize(
            QSize(kComposerIconPx, kComposerIconPx));
        m_quickAddSpeedSelector->setFixedWidth(kComposerControlPx);
        for (QComboBox *combo : {static_cast<QComboBox *>(m_quickAddAgentModelSelector),
                                 static_cast<QComboBox *>(m_quickAddTaskAssignee),
                                 static_cast<QComboBox *>(m_quickAddModeSelector),
                                 static_cast<QComboBox *>(m_quickAddSpeedSelector)})
            combo->setFixedHeight(kComposerControlPx);
    }
    auto *bottomBar = new QHBoxLayout;
    bottomBar->setContentsMargins(next ? 40 : 42, 4, next ? 4 : 6, next ? 4 : 0);
    bottomBar->setSpacing(next ? 4 : 5);
    bottomBar->addWidget(m_quickAddImageButton, 0, Qt::AlignBottom);
    bottomBar->addWidget(m_quickAddMicButton, 0, Qt::AlignBottom);
    bottomBar->addWidget(m_voiceLevelMeter, 0, Qt::AlignBottom);
    bottomBar->addWidget(m_quickAddAttachStrip, 0, Qt::AlignBottom);
    bottomBar->addStretch(1);
    bottomBar->addWidget(m_quickAddSlashButton, 0, Qt::AlignBottom);
    bottomBar->addWidget(agentBox, 0, Qt::AlignBottom);
    bottomBar->addStretch(1);
    bottomBar->addWidget(m_navCodexUsage, 0, Qt::AlignBottom);
    bottomBar->addWidget(m_navTokenUsage, 0, Qt::AlignBottom);

    auto *bottomBarHost = new QWidget;
    bottomBarHost->setObjectName("quickAddBottomBarHost");
    bottomBarHost->setLayout(bottomBar);
    syncQuickAddAgentControls();
    bottomBarHost->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *bottomBarScroll = new QScrollArea;
    bottomBarScroll->setObjectName("quickAddBottomBarScroll");
    bottomBarScroll->setWidget(bottomBarHost);
    bottomBarScroll->setWidgetResizable(true);
    bottomBarScroll->setFrameShape(QFrame::NoFrame);
    bottomBarScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bottomBarScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bottomBarScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    bottomBarScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bottomBarScroll->setMinimumWidth(0);
    bottomBarScroll->setFixedHeight(bottomBarHost->sizeHint().height());

    auto *promptWrapper = new QFrame;
    m_promptWrapper = promptWrapper;
    promptWrapper->setObjectName("promptWrapper");
    auto *chatPreview = new ChatPreviewLabel;
    chatPreview->onClicked = [this] {
        if (!promptChatRoom().isEmpty())
            return;
        const int sessionId = m_promptChatSessionId;
        if (sessionId >= 0 && findAgentSession(sessionId))
            showAgentSession(sessionId);
    };
    chatPreview->hide();
    m_promptChatPreview = chatPreview;
    constexpr int kPromptBarPx = 24;
    auto *promptTopSpacer = new QWidget;
    promptTopSpacer->setObjectName(QStringLiteral("promptTopSpacer"));
    promptTopSpacer->setFixedHeight(kPromptBarPx);
    promptTopSpacer->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *promptHandle =
        new FloatingPromptBar(promptWrapper, promptTopSpacer, chatPreview,
                              next ? 11 : 5);
    m_promptDragHandle = promptHandle;
    promptHandle->setObjectName(QStringLiteral("promptDragHandle"));
    promptHandle->setFixedHeight(kPromptBarPx);
    promptHandle->setCursor(Qt::OpenHandCursor);
    promptHandle->setToolTip(
        QStringLiteral("Drag to move the prompt; double-click to snap it back "
                       "to the corner"));
    promptHandle->installEventFilter(this);
    auto *promptHandleRow = new QHBoxLayout(promptHandle);
    promptHandleRow->setContentsMargins(FloatingPromptBar::kFadePx, 2,
                                        next ? 6 : 3, 2);
    promptHandleRow->setSpacing(4);
    const int handleButtonPx = next ? 20 : 16;

    m_promptResizeGrip = new QFrame;
    m_promptResizeGrip->setObjectName(QStringLiteral("promptResizeGrip"));
    m_promptResizeGrip->setFixedSize(12, 12);
    m_promptResizeGrip->setCursor(Qt::SizeBDiagCursor);
    m_promptResizeGrip->setToolTip(QStringLiteral("Drag to resize the prompt"));
    m_promptResizeGrip->installEventFilter(this);

    // Right of the resize grip: wipe a half-typed prompt in one click (adhoc
    // #1634). Same 16px footprint and its own object name as the reset/pop-out
    // pair at the other end of the strip, so it reads as one family of tiny
    // controls rather than a button borrowed from the toolbar below.
    m_promptClearButton = new QPushButton;
    m_promptClearButton->setObjectName(QStringLiteral("promptClearButton"));
    m_promptClearButton->setCursor(Qt::PointingHandCursor);
    m_promptClearButton->setFixedSize(handleButtonPx, handleButtonPx);
    setComposerIconTone(m_promptClearButton);
    setOcticon(m_promptClearButton, "x", next ? 14 : 10);
    m_promptClearButton->setToolTip(QStringLiteral("Clear the prompt"));
    connect(m_promptClearButton, &QPushButton::clicked, this, [this] {
        if (!m_issueQuickAdd)
            return;
        // Deleted through the cursor rather than clear(), which drops the undo
        // stack: a 16px target one gap away from the resize grip is easy to hit
        // by accident, and Ctrl+Z has to bring a long prompt back.
        m_issueQuickAdd->selectAll();
        m_issueQuickAdd->textCursor().removeSelectedText();
        m_issueQuickAdd->setFocus();
    });

    m_promptWorktreeCheck = new QPushButton;
    m_promptWorktreeCheck->setObjectName(QStringLiteral("promptWorktreeCheck"));
    m_promptWorktreeCheck->setCursor(Qt::PointingHandCursor);
    m_promptWorktreeCheck->setFixedSize(handleButtonPx, handleButtonPx);
    m_promptWorktreeCheck->setCheckable(true);
    m_promptWorktreeCheck->setChecked(true);
    m_promptWorktreeCheck->setAccessibleName(QStringLiteral("Own branch and worktree"));
    setComposerIconTone(m_promptWorktreeCheck);
    setOcticon(m_promptWorktreeCheck, "git-branch", next ? 14 : 12);
    const auto updateWorktreeTip = [button = m_promptWorktreeCheck](bool on) {
        button->setToolTip(
            on ? QStringLiteral("Connected: new agent runs get their own branch "
                                "and worktree. Click to work directly in the "
                                "repository's checkout.")
               : QStringLiteral("The next agent run works directly in the "
                                "repository's checkout, with no branch or "
                                "worktree (Claude Code and Codex only). This "
                                "turns back on once that prompt is sent."));
    };
    updateWorktreeTip(true);
    connect(m_promptWorktreeCheck, &QPushButton::toggled, this, updateWorktreeTip);

    m_promptCloudButton = new QPushButton;
    m_promptCloudButton->setObjectName(QStringLiteral("promptCloudButton"));
    m_promptCloudButton->setCursor(Qt::PointingHandCursor);
    m_promptCloudButton->setFixedSize(handleButtonPx, handleButtonPx);
    m_promptCloudButton->setCheckable(true);
    m_promptCloudButton->setChecked(
        QSettings().value(QStringLiteral("agents/composerRunInCloud"), false)
            .toBool());
    m_promptCloudButton->setAccessibleName(QStringLiteral("Run in the cloud"));
    setComposerIconTone(m_promptCloudButton);
    setOcticon(m_promptCloudButton, "cloud", next ? 14 : 12);
    const auto updateCloudTip = [button = m_promptCloudButton](bool on) {
        button->setToolTip(
            on ? QStringLiteral("New agent runs go to one of your ForkMesh "
                                "agent nodes in the cloud (Claude Code and "
                                "Codex). Click to run them on this desktop.")
               : QStringLiteral("New agent runs start on this desktop. Click "
                                "to send them to a ForkMesh agent node in the "
                                "cloud instead."));
    };
    updateCloudTip(m_promptCloudButton->isChecked());
    connect(m_promptCloudButton, &QPushButton::toggled, this,
            [updateCloudTip](bool on) {
                updateCloudTip(on);
                QSettings().setValue(QStringLiteral("agents/composerRunInCloud"),
                                     on);
            });

    m_promptVibeCheck = new QPushButton;
    m_promptVibeCheck->setObjectName(QStringLiteral("promptVibeCheck"));
    m_promptVibeCheck->setCursor(Qt::PointingHandCursor);
    m_promptVibeCheck->setFixedSize(handleButtonPx, handleButtonPx);
    m_promptVibeCheck->setCheckable(true);
    m_promptVibeCheck->setChecked(
        QSettings().value(QStringLiteral("agents/composerVibe"), false).toBool());
    m_promptVibeCheck->setAccessibleName(QStringLiteral("Vibe"));
    setComposerIconTone(m_promptVibeCheck);
    setOcticon(m_promptVibeCheck, "sparkle", next ? 14 : 12);
    const auto updateVibeTip = [button = m_promptVibeCheck](bool on) {
        button->setToolTip(
            on ? QStringLiteral("Vibe is on: when a new agent run finishes "
                                "cleanly, its branch is merged into the default "
                                "branch, its session, branch and worktree are "
                                "deleted, and the default branch is pushed. "
                                "Click to review runs by hand instead.")
               : QStringLiteral("Vibe: merge each new agent run into the "
                                "default branch when it finishes, delete its "
                                "session, branch and worktree, and push the "
                                "default branch."));
    };
    updateVibeTip(m_promptVibeCheck->isChecked());
    connect(m_promptVibeCheck, &QPushButton::toggled, this, [updateVibeTip](bool on) {
        updateVibeTip(on);
        QSettings().setValue(QStringLiteral("agents/composerVibe"), on);
    });
    promptHandleRow->addWidget(m_quickAddCharCount, 0);
    promptHandleRow->addWidget(m_promptWorktreeCheck, 0);
    promptHandleRow->addWidget(m_promptCloudButton, 0);
    promptHandleRow->addWidget(m_promptVibeCheck, 0);
    promptHandleRow->addSpacing(6);
    promptHandleRow->addWidget(m_promptClearButton, 0);

    m_promptResetButton = new QPushButton;
    m_promptResetButton->setObjectName(QStringLiteral("promptResetButton"));
    m_promptResetButton->setCursor(Qt::PointingHandCursor);
    m_promptResetButton->setFixedSize(handleButtonPx, handleButtonPx);
    setComposerIconTone(m_promptResetButton);
    setOcticon(m_promptResetButton, "sync", next ? 14 : 10);
    m_promptResetButton->setToolTip(
        QStringLiteral("Reset the prompt back to its default size and position"));
    connect(m_promptResetButton, &QPushButton::clicked, this,
            &MainWindow::resetPromptOverlayPlacement);
    promptHandleRow->addWidget(m_promptResetButton, 0);

    m_promptDetachButton = new QPushButton;
    m_promptDetachButton->setObjectName(QStringLiteral("promptDetachButton"));
    m_promptDetachButton->setCursor(Qt::PointingHandCursor);
    m_promptDetachButton->setFixedSize(handleButtonPx, handleButtonPx);
    setComposerIconTone(m_promptDetachButton);
    setOcticon(m_promptDetachButton, "screen-full", next ? 14 : 10);
    m_promptDetachButton->setToolTip(
        QStringLiteral("Pop the prompt out into its own window"));
    connect(m_promptDetachButton, &QPushButton::clicked, this,
            [this] { setPromptOverlayDetached(!m_promptOverlayDetached); });
    promptHandleRow->addWidget(m_promptDetachButton, 0);
    promptHandleRow->addSpacing(2);
    promptHandleRow->addWidget(m_promptResizeGrip, 0, Qt::AlignTop);

    auto *promptStack = new QVBoxLayout(promptWrapper);
    promptStack->setContentsMargins(0, 0, 0, 0);
    promptStack->setSpacing(0);
    promptStack->addWidget(promptTopSpacer, 0);
    auto *promptLayout = new QHBoxLayout;
    promptLayout->setContentsMargins(0, 0, 0, 0);
    promptLayout->setSpacing(0);
    promptStack->addLayout(promptLayout, 1);
    auto *promptLeftCol = new QVBoxLayout;
    promptLeftCol->setContentsMargins(0, 0, 0, 0);
    promptLeftCol->setSpacing(0);
    promptLeftCol->addWidget(chatPreview, 0);
    promptLeftCol->addWidget(quickAddInputHost, 1);
    promptLeftCol->addWidget(bottomBarScroll, 0);
    promptLayout->addLayout(promptLeftCol, 1);
    promptLayout->addLayout(sendColumn, 0);
    promptHandle->raise();

    promptWrapper->setMinimumWidth(0);
    promptWrapper->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *promptOverlayHost = new QWidget;
    m_promptOverlayHost = promptOverlayHost;
    promptOverlayHost->setObjectName(QStringLiteral("promptOverlayHost"));
    auto *promptOverlayLayout = new QVBoxLayout(promptOverlayHost);
    promptOverlayLayout->setContentsMargins(0, 0, 0, 0);
    promptOverlayLayout->addWidget(promptWrapper);
    promptOverlayHost->setMaximumWidth(560);
    promptOverlayHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_userAvatarNavButton->setParent(promptOverlayHost);
    m_navSolanaBalance->setParent(promptOverlayHost);
    m_navSolanaBalance->hide();
    m_userAvatarNavButton->setToolTip(
        QStringLiteral("Collapse the prompt overlay (Settings remains in the rail)"));
    m_userAvatarNavButton->raise();

    m_footerUpdateLog = new QTextEdit;
    m_footerUpdateLog->setObjectName("footerUpdateLog");
    m_footerUpdateLog->setReadOnly(true);
    m_footerUpdateLog->setFrameShape(QFrame::NoFrame);
    m_footerUpdateLog->document()->setDocumentMargin(0);
    m_footerUpdateLog->document()->setUndoRedoEnabled(false);
    m_footerUpdateLog->setLineWrapMode(QTextEdit::NoWrap);
    m_footerUpdateLog->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_footerUpdateLog->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_footerUpdateLog->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_footerUpdateLog->setMinimumWidth(0);
    m_footerUpdateLog->setToolTip(
        "Live log \xE2\x80\x94 click a line to open the full Log at it; scroll up "
        "to search back through recent history.");
    m_footerUpdateLog->viewport()->setCursor(Qt::PointingHandCursor);
    m_footerUpdateLog->viewport()->installEventFilter(this);
    styleFooterUpdateLog();
    if (!m_networkLog.isEmpty()) {
        const int from = qMax(0, m_networkLog.size() - kFooterLogSeedLines);
        QString seedHtml;
        QStringList seedLines; // the raw lines, one per rendered block
        for (int i = from; i < m_networkLog.size(); ++i) {
            const QString clean = m_networkLog.at(i).trimmed();
            if (clean.isEmpty())
                continue;
            seedHtml += QStringLiteral("<div>%1</div>")
                            .arg(footerLogLineHtml(clean, m_footerUpdateLog));
            seedLines << clean;
        }
        m_footerUpdateLog->setHtml(seedHtml);
        int seedIndex = 0;
        for (QTextBlock b = m_footerUpdateLog->document()->firstBlock();
             b.isValid() && seedIndex < seedLines.size(); b = b.next(), ++seedIndex)
            b.setUserData(new FooterLogLineData(seedLines.at(seedIndex)));
        m_footerUpdateLog->verticalScrollBar()->setValue(
            m_footerUpdateLog->verticalScrollBar()->maximum());
    } else {
        m_footerUpdateLog->setPlainText(QStringLiteral("ForkMesh ready"));
    }

    m_footerUpdateLog->installEventFilter(this);

    m_footerLogPauseButton = new QPushButton(m_footerUpdateLog);
    m_footerLogPauseButton->setObjectName("footerLogPauseButton");
    m_footerLogPauseButton->setCheckable(true);
    m_footerLogPauseButton->setFocusPolicy(Qt::NoFocus);
    m_footerLogPauseButton->setCursor(Qt::PointingHandCursor);
    m_footerLogPauseButton->setFixedSize(18, 14);
    m_footerLogPauseButton->setIconSize(QSize(8, 8));
    m_footerLogPauseButton->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "QPushButton#footerLogPauseButton{background:#f6f8fa;border:1px solid "
        "#d0d7de;border-radius:4px;color:#57606a;font-size:8px;padding:0;}"
        "QPushButton#footerLogPauseButton:hover{background:#eaeef2;color:#1f2328;}"
        "QPushButton#footerLogPauseButton:checked{background:%1;"
        "border-color:%2;color:%2;}")
        .arg(Theme::nextOrHex(Theme::Role::AccentSubtle, QStringLiteral("#ddf4e4")),
             Theme::nextOrHex(Theme::Role::AccentFg, QStringLiteral("#1a7f37")))));
    connect(m_footerLogPauseButton, &QPushButton::toggled, this,
            [this](bool paused) {
                m_footerLogScrollPaused = paused;
                updateFooterLogPauseButton();
                if (!paused && m_footerUpdateLog)
                    if (QScrollBar *bar = m_footerUpdateLog->verticalScrollBar())
                        bar->setValue(bar->maximum());
            });
    updateFooterLogPauseButton();
    positionFooterLogPauseButton();
    connect(m_footerUpdateLog->verticalScrollBar(), &QScrollBar::rangeChanged,
            this, [this] { positionFooterLogPauseButton(); });

    forkmesh::BackgroundActivity::setListener(
        [this](const forkmesh::BackgroundTicket &ticket, bool started) {
            const bool backgrounded =
                ticket.execution !=
                forkmesh::ActionTelemetry::Execution::UiBlocking;
            QMetaObject::invokeMethod(
                this,
                [this, ticket, backgrounded, started] {
                    noteBackgroundActivity(ticket, backgrounded, started);
                    noteBranchDiffTicket(ticket, started);
                },
                Qt::QueuedConnection);
        });

    auto *logPanel = new QFrame;
    logPanel->setObjectName(QStringLiteral("footerLogPanel"));
    auto *logPanelLayout = new QVBoxLayout(logPanel);
    logPanelLayout->setContentsMargins(1, 1, 1, 1);
    logPanelLayout->setSpacing(2);
    m_logActivityHeader = new LogActivityLights(LogActivityLights::Header,
                                                logPanel);
    logPanelLayout->addWidget(m_logActivityHeader, 0);
    logPanelLayout->addWidget(m_footerUpdateLog, 1);

    auto *leftRegion = new QWidget;
    m_footerLeftRegion = leftRegion;
    leftRegion->setObjectName(QStringLiteral("footerLeftRegion"));
    leftRegion->setMaximumWidth(720);
    leftRegion->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *leftRegionLayout = new QHBoxLayout(leftRegion);
    leftRegionLayout->setContentsMargins(0, 0, 0, 0);
    leftRegionLayout->setSpacing(8);
    leftRegionLayout->addWidget(logPanel, 1);

    m_logActivityLights = new LogActivityLights(LogActivityLights::Debug, dock);
    m_logActivityLights->onCategoryClicked = [this](const QString &category) {
        openFullLogForCategory(category);
    };
    m_logActivityLights->onWebsiteClicked = [this](const QString &statusId) {
        recheckWebsiteStatus(statusId);
        openWebsiteStatusTarget(statusId);
    };
    const QString stallTip = QStringLiteral(
        "Click to draft a fix-it prompt for recorded UI stalls; right-click "
        "for the captured backtraces.");
    for (LogActivityLights *lights : {m_logActivityLights, m_logActivityHeader}) {
        lights->setStallToolTip(stallTip);
        lights->onStallClicked = [this] { sendStallReportToComposer(); };
        lights->onStallContextMenu = [this] { showDiagnosticsDialog(); };
    }
    attachDebugActivityLights();

    auto *dockRow = new QHBoxLayout(dock);
    dockRow->setContentsMargins(8, 8, 8, 0);
    dockRow->setSpacing(8);
    dockRow->addWidget(leftRegion, 1, Qt::AlignLeft | Qt::AlignBottom);
    dockRow->addStretch(1);
    dockRow->addWidget(promptOverlayHost, 1, Qt::AlignRight | Qt::AlignBottom);

    m_promptAnchoredHeight = promptWrapper->sizeHint().height();
    promptWrapper->setProperty("baseAnchoredHeight", m_promptAnchoredHeight);
    dock->setFixedHeight(m_promptAnchoredHeight + 8);
    dock->setAttribute(Qt::WA_StyledBackground, false);
    setLogOverlayExpanded(false);

    QTimer::singleShot(1500, this, &MainWindow::refreshFooterWebsiteStatus);
#ifndef FORKMESH_WINDOW_TESTS
    QTimer::singleShot(3000, this, &MainWindow::refreshDesktopWebsiteProbes);
    QTimer::singleShot(2000, this, [this] { refreshAccountPackages(true); });
#endif

    updateVoiceInputButton();
    return dock;
}

void MainWindow::attachDebugActivityLights()
{
    if (!m_debugActivityLayout || !m_logActivityLights ||
        m_debugActivityLayout->indexOf(m_logActivityLights) >= 0)
        return;
    // The final item is the expanding spacer. Insert before it so a light
    // created after first paint lands beside All, exactly where it did when
    // the composer used to be constructed synchronously.
    m_debugActivityLayout->insertWidget(
        qMax(0, m_debugActivityLayout->count() - 1), m_logActivityLights, 0,
        Qt::AlignVCenter);
}

void MainWindow::setLogOverlayExpanded(bool expanded)
{
    m_logOverlayExpanded = expanded;
    if (m_footerLeftRegion)
        m_footerLeftRegion->setVisible(expanded);
    if (m_logActivityLights)
        m_logActivityLights->setExpanded(expanded);
    positionGlobalFooterOverlays();
}

void MainWindow::setPromptOverlayCollapsed(bool collapsed)
{
    m_promptOverlayCollapsed = collapsed;
    positionGlobalFooterOverlays();
}

namespace {
const char kPromptFloatingSetting[] = "prompt/floating";
const char kPromptFloatPosSetting[] = "prompt/floatPos";
const char kPromptFloatSizeSetting[] = "prompt/floatSize";
const char kPromptDetachedSetting[] = "prompt/detached";
const char kPromptDetachGeometrySetting[] = "prompt/detachGeometry";
constexpr int kPromptMinWidth = 280;
constexpr int kPromptMinHeight = 120;
} // namespace

void MainWindow::clampPromptOverlayIntoHost()
{
    if (!m_globalOverlayHost || !m_promptOverlayHost)
        return;
    if (m_promptOverlayHost->parentWidget() != m_globalOverlayHost)
        m_promptOverlayHost->setParent(m_globalOverlayHost);
    QSize size = m_promptOverlaySize.isValid()
                     ? m_promptOverlaySize
                     : QSize(qMin(560, m_globalOverlayHost->width()),
                             m_promptWrapper ? m_promptWrapper->sizeHint().height()
                                             : kPromptMinHeight);
    if (m_promptOverlayCollapsed) {
        size = QSize(34, 34);
    } else {
        size.setWidth(qBound(kPromptMinWidth, size.width(),
                             qMax(kPromptMinWidth, m_globalOverlayHost->width())));
        size.setHeight(qBound(kPromptMinHeight, size.height(),
                              qMax(kPromptMinHeight, m_globalOverlayHost->height())));
    }
    const QSize hostSize = m_globalOverlayHost->size();
    if (m_promptOverlayHostSize.isValid() && m_promptOverlayHostSize != hostSize) {
        m_promptOverlayPos += QPoint(hostSize.width() - m_promptOverlayHostSize.width(),
                                     hostSize.height() - m_promptOverlayHostSize.height());
    }
    m_promptOverlayHostSize = hostSize;
    const QPoint pos(
        qBound(0, m_promptOverlayPos.x(),
               qMax(0, m_globalOverlayHost->width() - size.width())),
        qBound(0, m_promptOverlayPos.y(),
               qMax(0, m_globalOverlayHost->height() - size.height())));
    m_promptOverlayPos = pos;
    m_promptOverlayHost->setGeometry(QRect(pos, size));
}

void MainWindow::setPromptOverlayDetached(bool detached)
{
    if (!m_promptOverlayHost || detached == m_promptOverlayDetached)
        return;
    auto *dockRow = m_footerDock
                        ? qobject_cast<QHBoxLayout *>(m_footerDock->layout())
                        : nullptr;
    if (detached) {
        if (!m_promptDetachWindow) {
            m_promptDetachWindow =
                new QWidget(this, Qt::Window | Qt::FramelessWindowHint);
            m_promptDetachWindow->setObjectName(QStringLiteral("promptDetachWindow"));
            m_promptDetachWindow->setAttribute(Qt::WA_TranslucentBackground, true);
            m_promptDetachWindow->setWindowTitle(QStringLiteral("ForkMesh Prompt"));
            auto *windowLayout = new QVBoxLayout(m_promptDetachWindow);
            windowLayout->setContentsMargins(0, 0, 0, 0);
            m_promptDetachWindow->installEventFilter(this);
        }
        const QSize previous = m_promptOverlayHost->size();
        if (dockRow)
            dockRow->removeWidget(m_promptOverlayHost);
        m_promptDetachWindow->layout()->addWidget(m_promptOverlayHost);
        m_promptOverlayDetached = true;
        m_promptOverlayCollapsed = false;
        m_promptOverlayHost->show();
        if (m_promptDetachGeometry.isValid())
            m_promptDetachWindow->setGeometry(m_promptDetachGeometry);
        else
            m_promptDetachWindow->resize(qMax(kPromptMinWidth, previous.width()),
                                         qMax(kPromptMinHeight, previous.height()));
        m_promptDetachWindow->show();
        m_promptDetachWindow->raise();
        m_promptDetachWindow->activateWindow();
    } else {
        if (m_promptDetachWindow) {
            m_promptDetachGeometry = m_promptDetachWindow->geometry();
            m_promptDetachWindow->layout()->removeWidget(m_promptOverlayHost);
            m_promptDetachWindow->hide();
        }
        m_promptOverlayDetached = false;
        if (m_promptOverlayFloating) {
            m_promptOverlayHost->setParent(m_globalOverlayHost);
        } else if (dockRow) {
            dockRow->addWidget(m_promptOverlayHost, 1,
                               Qt::AlignRight | Qt::AlignBottom);
        }
        m_promptOverlayHost->show();
    }
    if (m_promptDetachButton) {
        setOcticon(m_promptDetachButton,
                   m_promptOverlayDetached ? "sign-in" : "screen-full", 10);
        m_promptDetachButton->setToolTip(
            m_promptOverlayDetached
                ? QStringLiteral("Dock the prompt back into the window")
                : QStringLiteral("Pop the prompt out into its own window"));
    }
    positionGlobalFooterOverlays();
    savePromptOverlayPlacement();
}

bool MainWindow::promptDragLeavesWindow(const QPoint &globalCursor,
                                        const QRect &wanted) const
{
    if (!m_globalOverlayHost)
        return false;
    if (!QRect(mapToGlobal(QPoint()), size()).contains(globalCursor))
        return true;
    return !m_globalOverlayHost->rect().contains(wanted.center());
}

void MainWindow::tearOffPromptOverlay(const QPoint &globalCursor)
{
    if (!m_promptOverlayHost || !m_globalOverlayHost || !m_promptDragHandle)
        return;
    const QPoint grabOffset =
        m_promptPlacementGrab -
        m_globalOverlayHost->mapToGlobal(m_promptPlacementStartRect.topLeft());
    m_promptDragHandle->releaseMouse();
    m_promptPlacementDragging = false;
    m_promptPlacementResizing = false;
    m_promptOverlayPos = m_promptPlacementStartRect.topLeft();
    m_promptOverlaySize = m_promptPlacementStartRect.size();
    m_promptDetachGeometry = QRect(globalCursor - grabOffset,
                                   m_promptPlacementStartRect.size());
    setPromptOverlayDetached(true);
    if (!m_promptOverlayDetached || !m_promptDetachWindow)
        return;
    m_promptPlacementGrab = globalCursor;
    m_promptPlacementStartRect = m_promptDetachWindow->geometry();
    m_promptDetachDragging = true;
    m_promptDetachMoveAsked = false;
    m_promptDragHandle->setCursor(Qt::ClosedHandCursor);
    m_promptDragHandle->grabMouse();
}

void MainWindow::resetPromptOverlayPlacement()
{
    if (!m_promptOverlayHost)
        return;
    if (m_promptOverlayDetached)
        setPromptOverlayDetached(false);
    m_promptOverlayFloating = false;
    m_promptOverlayCollapsed = false;
    m_promptOverlaySize = QSize();
    m_promptOverlayPos = QPoint();
    m_promptOverlayHostSize = QSize();
    if (auto *dockRow = m_footerDock
                            ? qobject_cast<QHBoxLayout *>(m_footerDock->layout())
                            : nullptr) {
        if (dockRow->indexOf(m_promptOverlayHost) < 0)
            dockRow->addWidget(m_promptOverlayHost, 1,
                               Qt::AlignRight | Qt::AlignBottom);
        // Coming back through the layout reparents the panel, which hides it
        // and only queues the re-show for the next spin of the event loop. The
        // pass below would then lay the row out without it and mask the dock
        // down to a strip it is not in, so the prompt would come back
        // invisible until something else forced a relayout.
        m_promptOverlayHost->show();
    }
    positionGlobalFooterOverlays();
    savePromptOverlayPlacement();
}

void MainWindow::savePromptOverlayPlacement()
{
    QSettings settings;
    settings.setValue(QLatin1String(kPromptFloatingSetting), m_promptOverlayFloating);
    settings.setValue(QLatin1String(kPromptFloatPosSetting), m_promptOverlayPos);
    settings.setValue(QLatin1String(kPromptFloatSizeSetting), m_promptOverlaySize);
    settings.setValue(QLatin1String(kPromptDetachedSetting), m_promptOverlayDetached);
    if (m_promptOverlayDetached && m_promptDetachWindow)
        m_promptDetachGeometry = m_promptDetachWindow->geometry();
    settings.setValue(QLatin1String(kPromptDetachGeometrySetting),
                      m_promptDetachGeometry);
}

void MainWindow::loadPromptOverlayPlacement()
{
    if (!m_promptOverlayHost)
        return;
    QSettings settings;
    m_promptDetachGeometry =
        settings.value(QLatin1String(kPromptDetachGeometrySetting)).toRect();
    m_promptOverlayFloating = false;
    m_promptOverlayDetached = false;
    m_promptOverlaySize = QSize();
    m_promptOverlayPos = QPoint();
    savePromptOverlayPlacement();
}

bool MainWindow::handlePromptPlacementEvent(QObject *object, QEvent *event)
{
    const bool onHandle = object == m_promptDragHandle;
    const bool onGrip = object == m_promptResizeGrip;
    if (!onHandle && !onGrip)
        return false;
    if (!m_promptOverlayHost || !m_globalOverlayHost)
        return false;

    switch (event->type()) {
    case QEvent::MouseButtonDblClick:
        if (onHandle) {
            resetPromptOverlayPlacement();
            return true;
        }
        return false;
    case QEvent::MouseButtonPress: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton)
            return false;
        if (m_promptOverlayDetached) {
            if (!m_promptDetachWindow)
                return false;
            m_promptPlacementGrab = mouse->globalPosition().toPoint();
            m_promptPlacementStartRect = m_promptDetachWindow->geometry();
            if (onGrip) {
                QWindow *handle = m_promptDetachWindow->windowHandle();
                if (handle && handle->startSystemResize(Qt::TopEdge | Qt::RightEdge))
                    return true;
                m_promptDetachResizing = true;
                m_promptResizeGrip->grabMouse();
                return true;
            }
            m_promptDetachDragging = true;
            m_promptDetachMoveAsked = false;
            m_promptDragHandle->setCursor(Qt::ClosedHandCursor);
            m_promptDragHandle->grabMouse();
            return true;
        }
        if (!m_promptOverlayFloating) {
            const QPoint topLeft = m_globalOverlayHost->mapFromGlobal(
                m_promptOverlayHost->mapToGlobal(QPoint()));
            m_promptOverlayPos = topLeft;
            m_promptOverlaySize = m_promptOverlayHost->size();
            m_promptOverlayFloating = true;
            m_promptOverlayHostSize = QSize();
            if (auto *dockRow =
                    m_footerDock
                        ? qobject_cast<QHBoxLayout *>(m_footerDock->layout())
                        : nullptr)
                dockRow->removeWidget(m_promptOverlayHost);
            m_promptOverlayHost->setParent(m_globalOverlayHost);
            m_promptOverlayHost->show();
            positionGlobalFooterOverlays();
        }
        m_promptPlacementGrab = mouse->globalPosition().toPoint();
        m_promptPlacementStartRect =
            QRect(m_promptOverlayPos, m_promptOverlayHost->size());
        m_promptPlacementDragging = onHandle;
        m_promptPlacementResizing = onGrip;
        if (onHandle)
            m_promptDragHandle->setCursor(Qt::ClosedHandCursor);
        (onHandle ? m_promptDragHandle : m_promptResizeGrip)->grabMouse();
        return true;
    }
    case QEvent::MouseMove: {
        if (!m_promptPlacementDragging && !m_promptPlacementResizing &&
            !m_promptDetachDragging && !m_promptDetachResizing)
            return false;
        auto *mouse = static_cast<QMouseEvent *>(event);
        const QPoint cursor = mouse->globalPosition().toPoint();
        const QPoint delta = cursor - m_promptPlacementGrab;
        if (m_promptDetachResizing) {
            if (!m_promptDetachWindow)
                return true;
            const QRect start = m_promptPlacementStartRect;
            const QSize size(qMax(kPromptMinWidth, start.width() + delta.x()),
                             qMax(kPromptMinHeight, start.height() - delta.y()));
            m_promptDetachWindow->setGeometry(
                QRect(QPoint(start.left(), start.bottom() - size.height() + 1),
                      size));
            return true;
        }
        if (m_promptDetachDragging) {
            if (!m_promptDetachWindow)
                return true;
            if (!m_promptDetachMoveAsked) {
                m_promptDetachMoveAsked = true;
                QWindow *handle = m_promptDetachWindow->windowHandle();
                if (handle && handle->startSystemMove()) {
                    m_promptDragHandle->releaseMouse();
                    m_promptDragHandle->setCursor(Qt::OpenHandCursor);
                    m_promptDetachDragging = false;
                    return true;
                }
            }
            m_promptDetachWindow->setGeometry(
                QRect(m_promptPlacementStartRect.topLeft() + delta,
                      m_promptDetachWindow->size()));
            return true;
        }
        if (m_promptPlacementDragging) {
            const QRect wanted(m_promptPlacementStartRect.topLeft() + delta,
                               m_promptPlacementStartRect.size());
            if (promptDragLeavesWindow(cursor, wanted)) {
                tearOffPromptOverlay(cursor);
                return true;
            }
            m_promptOverlayPos = wanted.topLeft();
        } else {
            const QSize size(m_promptPlacementStartRect.width() + delta.x(),
                             m_promptPlacementStartRect.height() - delta.y());
            m_promptOverlaySize =
                QSize(qMax(kPromptMinWidth, size.width()),
                      qMax(kPromptMinHeight, size.height()));
            m_promptOverlayPos =
                QPoint(m_promptPlacementStartRect.left(),
                       m_promptPlacementStartRect.bottom() -
                           m_promptOverlaySize.height() + 1);
        }
        schedulePositionGlobalFooterOverlays();
        m_promptOverlayHost->raise();
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (!m_promptPlacementDragging && !m_promptPlacementResizing &&
            !m_promptDetachDragging && !m_promptDetachResizing)
            return false;
        (onHandle ? m_promptDragHandle : m_promptResizeGrip)->releaseMouse();
        if (m_promptDetachResizing) {
            m_promptDetachResizing = false;
            savePromptOverlayPlacement();
            return true;
        }
        if (m_promptDetachDragging) {
            m_promptDetachDragging = false;
            if (m_promptDragHandle)
                m_promptDragHandle->setCursor(Qt::OpenHandCursor);
            savePromptOverlayPlacement();
            return true;
        }
        if (m_footerOverlaysPositionQueued)
            positionGlobalFooterOverlays();
        m_promptPlacementDragging = false;
        m_promptPlacementResizing = false;
        if (m_promptDragHandle)
            m_promptDragHandle->setCursor(Qt::OpenHandCursor);
        m_promptOverlaySize = m_promptOverlayHost->size();
        savePromptOverlayPlacement();
        return true;
    }
    default:
        return false;
    }
}

void MainWindow::schedulePositionGlobalFooterOverlays()
{
    if (m_footerOverlaysPositionQueued)
        return;
    m_footerOverlaysPositionQueued = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_footerOverlaysPositionQueued = false;
            positionGlobalFooterOverlays();
        },
        Qt::QueuedConnection);
}

void MainWindow::positionGlobalFooterOverlays()
{
    if (!m_globalOverlayHost || !m_footerDock)
        return;
    // The pass re-margins the Agents panes, and those panes are watched for the
    // resize that follows: without the latch a placement could re-enter itself
    // through its own layout change.
    if (m_positioningFooterOverlays)
        return;
    m_positioningFooterOverlays = true;
    const auto latch =
        qScopeGuard([this] { m_positioningFooterOverlays = false; });
    const bool onAgents =
        (!m_sectionStack || m_sectionStack->currentIndex() == 0) &&
        m_repoDetailStack &&
        m_repoDetailStack->currentIndex() == kRepoAgentsTab;
    const bool anchoredPrompt =
        !m_promptOverlayFloating && !m_promptOverlayDetached;
    const bool dockAgentPrompt =
        onAgents && !m_promptOverlayCollapsed && anchoredPrompt;
    constexpr int kHorizontalMargin = 8;
    // The docked composer belongs to the transcript pane, not to the whole
    // Agents page: only that pane goes on to reserve room for it below, which
    // is what lets the session list keep running to the bottom of the window.
    // Pinning the composer to the pane's own left edge -- rather than to the
    // dock's midpoint -- keeps the two lined up wherever the splitter sits.
    const bool promptOverDetailPane =
        dockAgentPrompt && m_agentDetail && !m_agentDetail->isHidden() &&
        m_globalOverlayHost->isAncestorOf(m_agentDetail);
    const QRect detailRect =
        promptOverDetailPane
            ? QRect(m_agentDetail->mapTo(m_globalOverlayHost, QPoint()),
                    m_agentDetail->size())
            : QRect();

    if (m_footerLeftRegion)
        m_footerLeftRegion->setVisible(m_logOverlayExpanded && !onAgents);
    if (auto *dockRow = qobject_cast<QHBoxLayout *>(m_footerDock->layout())) {
        // On an open agent, the footer itself becomes the detail pane's exact
        // width. Dropping its horizontal inset keeps the prompt snug against
        // both transcript edges instead of leaving a narrow gutter.
        dockRow->setContentsMargins(promptOverDetailPane ? 0 : 8, 8,
                                    promptOverDetailPane ? 0 : 8, 0);
        QLayoutItem *spacerSlot = dockRow->itemAt(1);
        QSpacerItem *spacer = spacerSlot ? spacerSlot->spacerItem() : nullptr;
        const QSizePolicy::Policy spacerPolicy =
            promptOverDetailPane ? QSizePolicy::Fixed : QSizePolicy::Minimum;
        bool rowChanged = false;
        if (spacer &&
            (spacer->sizePolicy().horizontalPolicy() != spacerPolicy ||
             spacer->sizePolicy().verticalPolicy() != QSizePolicy::Minimum ||
             spacer->sizeHint() != QSize(0, 0))) {
            spacer->changeSize(0, 0, spacerPolicy, QSizePolicy::Minimum);
            rowChanged = true;
        }
        dockRow->setStretch(1, promptOverDetailPane ? 0 : 1); // transparent spacer
        const Qt::Alignment promptAlignment =
            dockAgentPrompt ? Qt::AlignBottom : Qt::AlignRight | Qt::AlignBottom;
        const int promptSlot = dockRow->indexOf(m_promptOverlayHost);
        QLayoutItem *promptItem =
            promptSlot >= 0 ? dockRow->itemAt(promptSlot) : nullptr;
        if (promptItem && promptItem->alignment() != promptAlignment) {
            dockRow->setAlignment(m_promptOverlayHost, promptAlignment);
            rowChanged = true;
        }
        if (rowChanged)
            dockRow->invalidate();
    }
    if (m_promptOverlayHost && m_promptWrapper) {
        const bool showPrompt = !m_promptOverlayCollapsed || m_promptOverlayDetached;
        m_promptWrapper->setVisible(showPrompt);
        if (!showPrompt) {
            m_promptOverlayHost->setFixedSize(34, 34);
            m_userAvatarNavButton->setToolTip(
                QStringLiteral("Show the prompt overlay"));
        } else if (anchoredPrompt) {
            m_issueQuickAdd->setMaximumHeight(m_issueQuickAdd->minimumHeight());
            m_promptOverlayHost->setMinimumSize(0, 0);
            m_promptOverlayHost->setMaximumSize(
                dockAgentPrompt ? QWIDGETSIZE_MAX : 560, QWIDGETSIZE_MAX);
            m_promptOverlayHost->setFixedHeight(m_promptAnchoredHeight);
            m_promptOverlayHost->setSizePolicy(QSizePolicy::Expanding,
                                               QSizePolicy::Fixed);
            m_userAvatarNavButton->setToolTip(
                dockAgentPrompt
                    ? QStringLiteral("The prompt is docked on the Agents page")
                    : QStringLiteral("Collapse the prompt overlay"));
        } else {
            m_promptOverlayHost->setMinimumSize(0, 0);
            m_promptOverlayHost->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            m_promptOverlayHost->setSizePolicy(QSizePolicy::Expanding,
                                               QSizePolicy::Expanding);
            m_issueQuickAdd->setMaximumHeight(QWIDGETSIZE_MAX);
            m_userAvatarNavButton->setToolTip(
                m_promptOverlayDetached
                    ? QStringLiteral("The prompt is in its own window")
                    : QStringLiteral("Collapse the prompt overlay"));
        }
    }
    forkmesh::ui::NextAccess::placeWorkspaceComposer(*this, promptOverDetailPane);
    if (m_promptOverlayFloating && !m_promptOverlayDetached && m_promptOverlayHost) {
        clampPromptOverlayIntoHost();
        m_promptOverlayHost->show();
        m_promptOverlayHost->raise();
    }
    // setGeometry() clamps to the dock's pinned height, so the top edge has to
    // be measured against that same clamped value. A floating or detached
    // prompt leaves the row empty and collapses the layout's size hint to the
    // margins: taking the hint at face value parks the dock a whole panel
    // height below the host, where all but a sliver of the strip is clipped
    // away -- and a prompt snapped back into it lands in the clipped part and
    // vanishes.
    const int height = qBound(m_footerDock->minimumHeight(),
                              m_footerDock->sizeHint().height(),
                              m_footerDock->maximumHeight());
    // Reserve the band on the pane the composer actually covers. Insetting the
    // whole page instead lifts the session list off the bottom as well, which
    // strands a dead white rectangle in the bottom-left corner where rows
    // should still be scrolling.
    const auto reserveComposerBand = [](QWidget *pane, int base, int extra) {
        if (!pane || !pane->layout())
            return;
        const QMargins old = pane->layout()->contentsMargins();
        if (old.bottom() != base + extra)
            pane->layout()->setContentsMargins(old.left(), old.top(),
                                               old.right(), base + extra);
    };
    reserveComposerBand(m_agentsPage, 0,
                        dockAgentPrompt && !promptOverDetailPane ? height : 0);
    reserveComposerBand(m_agentDetail, m_agentDetailBaseBottomMargin,
                        promptOverDetailPane ? height : 0);
    m_footerDock->setGeometry(
        promptOverDetailPane ? detailRect.x() : kHorizontalMargin,
        qMax(0, m_globalOverlayHost->height() - height),
        promptOverDetailPane
            ? detailRect.width()
            : qMax(0, m_globalOverlayHost->width() - 2 * kHorizontalMargin),
        height);
    m_footerDock->raise();
    if (m_promptOverlayHost && m_userAvatarNavButton) {
        const int x = m_promptOverlayCollapsed ? 4 : 8;
        const int y = qMax(0, m_promptOverlayHost->height() -
                                  m_userAvatarNavButton->height() - 4);
        m_userAvatarNavButton->move(x, y);
        m_userAvatarNavButton->raise();
    }
    if (QLayout *layout = m_footerDock->layout())
        layout->activate();
    QRegion interactive;
    // isHidden() rather than isVisible(): a panel that was just re-added to the
    // row is shown but not yet visible when the dock itself is still hidden,
    // and reading it as invisible here would mask away the very widget this
    // pass is putting back.
    if (m_footerLeftRegion && !m_footerLeftRegion->isHidden())
        interactive += m_footerLeftRegion->geometry();
    if (anchoredPrompt && m_promptOverlayHost && !m_promptOverlayHost->isHidden())
        interactive += m_promptOverlayHost->geometry();
    m_footerDock->setMask(interactive);
    // An empty region clears the mask instead of masking everything away, so a
    // strip with nothing in it has to be hidden outright or it swallows every
    // click over the bottom of the page.
    m_footerDock->setVisible(!interactive.isEmpty());
    if (m_promptOverlayFloating && !m_promptOverlayDetached && m_promptOverlayHost)
        m_promptOverlayHost->raise();
    positionGenieSpider();
}

namespace {
struct DesktopEdgeProbe {
    const char *id;
    const char *path;
    const char *label;
    const char *marker;     // identity of the expected document
    const char *what;       // how the row names itself in a reason line
    const char *mergesInto;
};

const QVector<DesktopEdgeProbe> &desktopEdgeProbes()
{
    static const QVector<DesktopEdgeProbe> probes = {
        {"desktop_website", "/dashboard", "Website loaded from this desktop", "forkmesh",
         "The site", "website"},
        {"desktop_status_page", "/status", "Status page loaded from this desktop",
         "status-title", "The /status page", "status_page"},
    };
    return probes;
}

int desktopEdgeProbeRank(const QString &id)
{
    const QVector<DesktopEdgeProbe> &probes = desktopEdgeProbes();
    for (int i = 0; i < probes.size(); ++i) {
        if (id == QLatin1String(probes.at(i).id))
            return i;
    }
    return int(probes.size());
}

const DesktopEdgeProbe *desktopEdgeProbe(const QString &id)
{
    for (const DesktopEdgeProbe &probe : desktopEdgeProbes()) {
        if (id == QLatin1String(probe.id))
            return &probe;
    }
    return nullptr;
}

QStringList desktopProbesForStatus(const QString &statusId)
{
    QStringList ids;
    for (const DesktopEdgeProbe &probe : desktopEdgeProbes()) {
        if (statusId == QLatin1String(probe.id) ||
            statusId == QLatin1String(probe.mergesInto))
            ids.append(QString::fromLatin1(probe.id));
    }
    return ids;
}

QString cloudflareEdgeErrorName(int httpStatus)
{
    switch (httpStatus) {
    case 520: return QStringLiteral("web server returned an unknown error");
    case 521: return QStringLiteral("web server is down");
    case 522: return QStringLiteral("connection timed out");
    case 523: return QStringLiteral("origin is unreachable");
    case 524: return QStringLiteral("a timeout occurred");
    case 525: return QStringLiteral("SSL handshake failed");
    case 526: return QStringLiteral("invalid SSL certificate");
    case 527: return QStringLiteral("Railgun listener to origin error");
    case 530: return QStringLiteral("origin DNS error");
    default: return QString();
    }
}

bool looksLikeCloudflareErrorDocument(const QString &lowered)
{
    if (!lowered.contains(QLatin1String("cloudflare")))
        return false;
    return lowered.contains(QLatin1String("cf-error")) ||
           lowered.contains(QLatin1String("ray id")) ||
           lowered.contains(QLatin1String("error code")) ||
           lowered.contains(QLatin1String("attention required")) ||
           lowered.contains(QLatin1String("just a moment"));
}

struct DesktopEdgeVerdict {
    QString status;
    QString reason;
};

constexpr int kDesktopProbeHistoryMax = 10;
constexpr qint64 kDesktopProbeHistoryMs = 10 * 60 * 1000;
// How long a page check waits. Qt ends a transfer that outlives it with the
// same "Operation canceled" as a request this desktop withdrew itself.
constexpr int kDesktopProbeTimeoutMs = 8000;
// Both page checks and a few window activations share one firewall ping.
constexpr qint64 kLocalFirewallAlertGapMs = 10 * 60 * 1000;

// The footer status dots are read at launch, when a dot is rechecked and when
// the window is activated -- never on a timer. Activation re-reads only once
// the last answer is this old: /api/status is a Worker request against the
// daily cap and the desktop probes are real page loads through the redirect
// chain (four requests for two dots), for every running desktop. A row that
// is not operational shortens the floor so a recovery shows on the next
// return to the window instead of a quarter of an hour later.
constexpr qint64 kFooterStatusHealthyFloorMs = 15 * 60 * 1000;
constexpr qint64 kFooterStatusUnhealthyFloorMs = 2 * 60 * 1000;
constexpr int kCustomMonitorTimeoutMs = 10000;
const QString kStatusPageId = QStringLiteral("status-page");
const QString kCustomMonitorPrefix = QStringLiteral("custom:");

int websiteStatusSeverity(const QString &status)
{
    if (status == QLatin1String("down"))
        return 3;
    if (status == QLatin1String("degraded"))
        return 2;
    if (status == QLatin1String("operational"))
        return 1;
    return 0;
}

QString worseWebsiteStatus(const QString &left, const QString &right)
{
    return websiteStatusSeverity(right) > websiteStatusSeverity(left) ? right
                                                                     : left;
}

DesktopEdgeVerdict mergeDesktopEdgeHistory(const DesktopEdgeVerdict &sample,
                                           const QString &what, int downs,
                                           int samples, qint64 lastDownTs)
{
    if (downs <= 0 || samples <= 0 || sample.status == QLatin1String("down"))
        return sample;
    const QString tally =
        QStringLiteral("%1 failed %2 of the last %3 checks from this desktop "
                       "(most recently at %4).")
            .arg(what)
            .arg(downs)
            .arg(samples)
            .arg(QDateTime::fromMSecsSinceEpoch(lastDownTs)
                     .toLocalTime()
                     .toString(QStringLiteral("HH:mm")));
    if (downs * 2 >= samples) {
        return {QStringLiteral("down"),
                tally + QStringLiteral(" A page that happens to load does not "
                                       "make it reachable.")};
    }
    return {QStringLiteral("degraded"),
            tally + QStringLiteral(" This check answered, but the site is not "
                                   "serving reliably.")};
}

QString apiHostFor(const QString &host)
{
    if (host.startsWith(QLatin1String("api."), Qt::CaseInsensitive))
        return host;
    QString bare = host;
    if (bare.startsWith(QLatin1String("www."), Qt::CaseInsensitive))
        bare = bare.mid(4);
    if (!bare.contains(QLatin1Char('.')) || !QHostAddress(bare).isNull())
        return host;
    return QStringLiteral("api.") + bare;
}

// The admin console answers on a secret path each deployment chooses (the
// Worker's ADMIN_PATH), so it is deliberately never baked into this public
// source. An operator can point their desktop straight at it with
// FORKMESH_ADMIN_PATH; otherwise the relay hands a signed-in admin their own
// URL (see MainWindow::openAdminErrorConsole).
QString configuredAdminPath()
{
    QString path = qEnvironmentVariable("FORKMESH_ADMIN_PATH").trimmed();
    if (path.isEmpty())
        path = qEnvironmentVariable("ADMIN_PATH").trimmed();
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

bool osReportsOffline()
{
    const auto *netInfo = QNetworkInformation::instance();
    return netInfo && netInfo->reachability() ==
                          QNetworkInformation::Reachability::Disconnected;
}

// Per-program firewalls key their rules on the executable's path, so a rebuilt
// or moved ForkMesh is a stranger they can deny outright -- its DNS lookups
// too, which Qt reports as "Host not found". Name the one that is running so
// the alert can say where to allow ForkMesh.
QString runningAppFirewallName()
{
#ifdef Q_OS_LINUX
    static const struct {
        const char *comm; // /proc/<pid>/comm, at most 15 characters
        const char *name;
    } known[] = {
        {"opensnitchd", "OpenSnitch"},
        {"portmaster-core", "Portmaster"},
    };
    const QStringList pids = QDir(QStringLiteral("/proc"))
                                 .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : pids) {
        if (pid.isEmpty() || !pid.at(0).isDigit())
            continue;
        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
        if (!comm.open(QIODevice::ReadOnly))
            continue;
        const QByteArray name = comm.readAll().trimmed();
        for (const auto &entry : known) {
            if (name == entry.comm)
                return QString::fromLatin1(entry.name);
        }
    }
#endif
    return QString();
}

DesktopEdgeVerdict gradeDesktopEdgeProbe(const DesktopEdgeProbe &probe,
                                         const QString &host, int httpStatus,
                                         const QByteArray &body,
                                         const QString &transportError,
                                         bool osOffline, bool appOffline)
{
    const QString what = QString::fromLatin1(probe.what);
    if (!transportError.isEmpty()) {
        if (transportError.contains(QLatin1String("firewall blocked"),
                                    Qt::CaseInsensitive)) {
            return {QStringLiteral("unknown"),
                    QStringLiteral("This desktop's own firewall blocked the "
                                   "check, so %1 was not loaded from here.")
                        .arg(what.toLower())};
        }
        // Offline mode never sent the probe, so this desktop observed nothing
        // about the site. Grading it "down" turned the reader's own switch
        // into a public outage on the footer lights (issue #712).
        if (appOffline) {
            return {QStringLiteral("unknown"),
                    QStringLiteral("This desktop is in offline mode, so %1 was "
                                   "not checked from here.")
                        .arg(host)};
        }
        if (osOffline) {
            return {QStringLiteral("unknown"),
                    QStringLiteral("This desktop is offline, so %1 could not be "
                                   "checked from here.")
                        .arg(host)};
        }
        // Defensive fallback for network-manager substitutes that preserve
        // the suppression error text but not its reply attribute. Backoff is
        // a local scheduling decision, not evidence that the public page is
        // down, so it may grade degraded but must never raise an outage.
        if (transportError.contains(QLatin1String("ForkMesh relay"),
                                    Qt::CaseInsensitive) &&
            transportError.contains(QLatin1String("backing off"),
                                    Qt::CaseInsensitive)) {
            return {QStringLiteral("degraded"),
                    QStringLiteral("This desktop is waiting for %1's relay "
                                   "cooldown before checking %2 again.")
                        .arg(host, what.toLower())};
        }
        // Qt can surface an HTTP/2 stream teardown from its pooled connection
        // even though the public site is healthy (for example, when the peer
        // closes an idle stream while this probe is being scheduled).  That
        // says nothing about the edge's availability, so keep the row grey and
        // do not raise a false outage alert from this desktop.
        if (transportError.contains(QLatin1String("half-closed stream"),
                                    Qt::CaseInsensitive) ||
            transportError.contains(QLatin1String("HTTP/2 protocol"),
                                    Qt::CaseInsensitive) ||
            transportError.contains(QLatin1String("stream was reset"),
                                    Qt::CaseInsensitive) ||
            transportError.contains(QLatin1String("network session failed"),
                                    Qt::CaseInsensitive) ||
            transportError.contains(QLatin1String("temporary network failure"),
                                    Qt::CaseInsensitive)) {
            return {QStringLiteral("unknown"),
                    QStringLiteral("This desktop's HTTP session ended before "
                                   "%1 could be verified: %2")
                        .arg(what.toLower(), transportError)};
        }
        // "Operation canceled" is this desktop aborting its own request: the
        // app is restarting, or a newer probe replaced this one. A site that
        // really hangs is reported as "Operation timed out" and still grades
        // down; a cancel observed nothing, so it must not raise an outage.
        if (transportError.contains(QLatin1String("Operation canceled"),
                                    Qt::CaseInsensitive)) {
            return {QStringLiteral("unknown"),
                    QStringLiteral("This desktop stopped its own check of %1 "
                                   "before it finished.")
                        .arg(what.toLower())};
        }
        return {QStringLiteral("down"),
                QStringLiteral("%1 could not be reached from this desktop: %2")
                    .arg(what, transportError)};
    }

    const QString lowered = QString::fromUtf8(body.left(64 * 1024)).toLower();
    const bool branded = looksLikeCloudflareErrorDocument(lowered);
    const QString edgeError = cloudflareEdgeErrorName(httpStatus);
    if (!edgeError.isEmpty()) {
        return {QStringLiteral("down"),
                QStringLiteral("Cloudflare answered %1 with HTTP %2 — %3.")
                    .arg(what)
                    .arg(httpStatus)
                    .arg(edgeError)};
    }
    if (httpStatus == 429 || lowered.contains(QLatin1String("error 1015"))) {
        return {QStringLiteral("degraded"),
                QStringLiteral("Cloudflare rate-limited this desktop (HTTP %1) "
                               "instead of serving %2.")
                    .arg(httpStatus)
                    .arg(what.toLower())};
    }
    if ((httpStatus == 403 || httpStatus == 503) && branded) {
        return {QStringLiteral("degraded"),
                QStringLiteral("Cloudflare challenged or blocked this desktop "
                               "(HTTP %1) instead of serving %2.")
                    .arg(httpStatus)
                    .arg(what.toLower())};
    }
    if (httpStatus >= 500) {
        return {QStringLiteral("down"),
                QStringLiteral("%1 returned HTTP %2 to this desktop.")
                    .arg(what)
                    .arg(httpStatus)};
    }
    if (httpStatus >= 300 && httpStatus < 400) {
        return {QStringLiteral("degraded"),
                QStringLiteral("%1 redirected this desktop with HTTP %2; the "
                               "redirect target could not be verified.")
                    .arg(what)
                    .arg(httpStatus)};
    }
    if (httpStatus != 200 && httpStatus != 206) {
        return {QStringLiteral("down"),
                QStringLiteral("%1 returned HTTP %2 to this desktop.")
                    .arg(what)
                    .arg(httpStatus)};
    }
    if (branded) {
        return {QStringLiteral("down"),
                QStringLiteral("Cloudflare served an error document for %1 "
                               "instead of the page.")
                    .arg(what.toLower())};
    }
    if (!lowered.contains(QLatin1String("<!doctype html")) ||
        !lowered.contains(QLatin1String(probe.marker))) {
        return {QStringLiteral("degraded"),
                QStringLiteral("%1 answered HTTP %2 from this desktop, but the "
                               "document was not the expected page.")
                    .arg(what)
                    .arg(httpStatus)};
    }
    return {QStringLiteral("operational"), QString()};
}
} // namespace

bool MainWindow::applyFooterWebsiteStatusPayload(const QJsonObject &payload)
{
    if (!payload.value(QStringLiteral("ok")).toBool())
        return false;
    const QJsonArray systems = payload.value(QStringLiteral("systems")).toArray();
    if (systems.isEmpty())
        return false;

    const qint64 payloadNow = static_cast<qint64>(
        payload.value(QStringLiteral("now")).toDouble(
            QDateTime::currentMSecsSinceEpoch()));
    QList<FooterStatusRow> statuses;
    statuses.reserve(systems.size());
    for (const QJsonValue &value : systems) {
        const QJsonObject system = value.toObject();
        FooterStatusRow result;
        result.id = system.value(QStringLiteral("id")).toString().trimmed();
        result.label = system.value(QStringLiteral("label")).toString().trimmed();
        if (result.id.isEmpty())
            continue;
        if (result.label.isEmpty())
            result.label = result.id;

        const QJsonArray minutes = system.value(QStringLiteral("minutes")).toArray();
        for (int i = minutes.size() - 1; i >= 0; --i) {
            const QJsonObject minute = minutes.at(i).toObject();
            const QString state =
                minute.value(QStringLiteral("status")).toString().trimmed().toLower();
            const qint64 minuteTs = static_cast<qint64>(
                minute.value(QStringLiteral("minuteTs")).toDouble());
            if (state.isEmpty() || state == QLatin1String("future") ||
                (minuteTs > 0 && minuteTs > payloadNow))
                continue;
            result.status = state;
            result.reason = minute.value(QStringLiteral("reason")).toString().trimmed();
            result.minuteTs = minuteTs;
            break;
        }
        if (result.status.isEmpty()) {
            result.status = QStringLiteral("unknown");
            result.reason = QStringLiteral("No completed status sample is available.");
        } else if (result.status != QLatin1String("operational") &&
                   result.status != QLatin1String("degraded") &&
                   result.status != QLatin1String("down")) {
            result.status = QStringLiteral("unknown");
        }
        statuses.append(result);
    }
    if (statuses.isEmpty())
        return false;
    m_footerRelayStatuses = statuses;
    publishFooterWebsiteStatuses();
    return true;
}

bool MainWindow::statusSystemShown(const QString &systemId) const
{
    if (systemId.startsWith(kCustomMonitorPrefix))
        return true;
    if (!m_packageRegistry || !m_packageRegistry->isReady())
        return true;
    return m_packageRegistry->statusMonitorShows(systemId);
}

void MainWindow::syncStatusPagePackage(bool installed)
{
    if (installed && !m_statusCustomMonitorsLoaded)
        loadCachedStatusMonitors();
    QSet<QString> shownProbes;
    for (const DesktopEdgeProbe &probe : desktopEdgeProbes()) {
        const QString id = QString::fromLatin1(probe.id);
        if (installed && statusSystemShown(id))
            shownProbes.insert(id);
    }
    const QSet<QString> newlyShown =
        m_statusShownDesktopProbesKnown
            ? QSet<QString>(shownProbes).subtract(m_statusShownDesktopProbes)
            : QSet<QString>();
    m_statusShownDesktopProbes = shownProbes;
    m_statusShownDesktopProbesKnown = true;
    if (installed == m_statusPagePackageInstalled) {
        publishFooterWebsiteStatuses();
        for (const QString &id : newlyShown)
            probeDesktopWebsite(id);
        return;
    }
    m_statusPagePackageInstalled = installed;
    if (!installed) {
        m_statusCustomMonitors.clear();
        m_statusCustomMonitorsLoaded = false;
        m_footerCustomStatuses.clear();
        m_statusMonitorsReadMs = 0;
        m_statusMonitorsError.clear();
        m_statusMonitorDraftName.clear();
        m_statusMonitorDraftUrl.clear();
    }
    publishFooterWebsiteStatuses();
    if (!installed)
        return;
    refreshFooterWebsiteStatus();
    refreshDesktopWebsiteProbes();
    refreshStatusMonitors(true);
}

void MainWindow::publishFooterWebsiteStatuses()
{
    QList<LogActivityLights::WebsiteStatus> rows;
    if (!m_statusPagePackageInstalled) {
        if (m_logActivityLights)
            m_logActivityLights->setWebsiteStatuses(rows);
        if (m_logActivityHeader)
            m_logActivityHeader->setWebsiteStatuses(rows);
        positionGlobalFooterOverlays();
        return;
    }
    rows.reserve(m_footerRelayStatuses.size() + m_footerDesktopStatuses.size() +
                 m_statusCustomMonitors.size());
    for (const FooterStatusRow &source : m_footerRelayStatuses) {
        if (!statusSystemShown(source.id))
            continue;
        LogActivityLights::WebsiteStatus row;
        row.id = source.id;
        row.label = source.label;
        row.status = source.status;
        row.reason = source.reason;
        row.minuteTs = source.minuteTs;
        row.local = source.local;
        row.checking = m_footerWebsiteStatusInFlight;
        rows.append(row);
    }
    for (const FooterStatusRow &source : m_footerDesktopStatuses) {
        if (!statusSystemShown(source.id))
            continue;
        const DesktopEdgeProbe *probe = desktopEdgeProbe(source.id);
        const QString target =
            probe ? QString::fromLatin1(probe->mergesInto) : QString();
        int merged = -1;
        for (int i = 0; i < rows.size() && merged < 0; ++i) {
            if (!target.isEmpty() && rows.at(i).id == target)
                merged = i;
        }
        const bool checking = m_desktopProbesInFlight.contains(source.id);
        if (merged < 0) {
            LogActivityLights::WebsiteStatus row;
            row.id = source.id;
            row.label = source.label;
            row.status = source.status;
            row.reason = source.reason;
            row.minuteTs = source.minuteTs;
            row.local = true;
            row.checking = checking;
            rows.append(row);
            continue;
        }
        LogActivityLights::WebsiteStatus &row = rows[merged];
        row.status = worseWebsiteStatus(row.status, source.status);
        row.localStatus = source.status;
        row.localReason = source.reason;
        row.localCheckedTs = source.minuteTs;
        row.checking = row.checking || checking;
    }
    for (const StatusCustomMonitor &monitor :
         std::as_const(m_statusCustomMonitors)) {
        LogActivityLights::WebsiteStatus row;
        row.id = kCustomMonitorPrefix + monitor.id;
        row.label = monitor.name;
        row.status = QStringLiteral("unknown");
        row.reason = QStringLiteral("%1 has not been checked from this "
                                    "desktop yet.")
                         .arg(monitor.url);
        row.local = true;
        for (const FooterStatusRow &source :
             std::as_const(m_footerCustomStatuses)) {
            if (source.id != row.id)
                continue;
            row.status = source.status;
            row.reason = source.reason;
            row.minuteTs = source.minuteTs;
        }
        row.checking = m_desktopProbesInFlight.contains(row.id);
        rows.append(row);
    }
    const bool onlyProblems =
        m_packageRegistry && m_packageRegistry->isReady() &&
        m_packageRegistry
            ->declaredSetting(kStatusPageId, QStringLiteral("onlyProblems"))
            .toBool();
    if (onlyProblems) {
        rows.removeIf([](const LogActivityLights::WebsiteStatus &row) {
            return row.status == QLatin1String("operational");
        });
    }
    if (rows.isEmpty() && m_footerRelayStatuses.isEmpty() &&
        m_footerDesktopStatuses.isEmpty() && m_statusCustomMonitors.isEmpty())
        return;
    if (m_logActivityLights)
        m_logActivityLights->setWebsiteStatuses(rows);
    if (m_logActivityHeader)
        m_logActivityHeader->setWebsiteStatuses(rows);
    positionGlobalFooterOverlays();
}

QUrl MainWindow::websiteStatusTargetUrl(const QString &statusId) const
{
    if (statusId.startsWith(kCustomMonitorPrefix)) {
        const QString monitorId = statusId.mid(kCustomMonitorPrefix.size());
        for (const StatusCustomMonitor &monitor :
             std::as_const(m_statusCustomMonitors)) {
            if (monitor.id == monitorId)
                return QUrl(monitor.url);
        }
        return QUrl();
    }
    QUrl url = catalogApiUrl();
    if (!url.isValid() || url.host().isEmpty())
        return QUrl();
    url.setPath(QStringLiteral("/"));
    url.setQuery(QString());
    url.setFragment(QString());
    if (statusId == QLatin1String("status_page") ||
        statusId == QLatin1String("desktop_status_page")) {
        url.setPath(QStringLiteral("/status"));
    } else if (statusId == QLatin1String("api")) {
        url.setHost(apiHostFor(url.host()));
    } else if (statusId == QLatin1String("flagship_repository")) {
        url.setPath(QStringLiteral("/forkmesh/forkmesh"));
    } else if (statusId == QLatin1String("errors")) {
        const QString adminPath = configuredAdminPath();
        if (adminPath.isEmpty())
            return QUrl();
        url.setPath(QLatin1Char('/') + adminPath);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("table"),
                           QStringLiteral("error_log"));
        url.setQuery(query);
    }
    return url;
}

void MainWindow::openWebsiteStatusTarget(const QString &statusId)
{
    if (statusId == QLatin1String("errors")) {
        openAdminErrorConsole();
        return;
    }
    const QUrl url = websiteStatusTargetUrl(statusId);
    if (url.isValid() && !url.host().isEmpty())
        QDesktopServices::openUrl(url);
}

void MainWindow::recheckWebsiteStatus(const QString &statusId)
{
    if (statusId.startsWith(kCustomMonitorPrefix)) {
        probeCustomStatusMonitor(statusId.mid(kCustomMonitorPrefix.size()));
        return;
    }
    refreshFooterWebsiteStatus();
    for (const QString &id : desktopProbesForStatus(statusId))
        probeDesktopWebsite(id);
}

#ifdef FORKMESH_WINDOW_TESTS
QStringList MainWindow::testWebsiteRecheckProbes(const QString &statusId) const
{
    return desktopProbesForStatus(statusId);
}
#endif

// The Worker-errors dot opens the admin error log itself. Its console sits
// behind a secret, deployment-configured path, so unless this desktop was told
// that path outright it asks the relay for the signed-in admin's own URL. An
// account that is not an admin gets the public /status page instead — the only
// error view it could open anyway.
void MainWindow::openAdminErrorConsole()
{
    const QUrl configured = websiteStatusTargetUrl(QStringLiteral("errors"));
    if (configured.isValid() && !configured.host().isEmpty()) {
        QDesktopServices::openUrl(configured);
        return;
    }
    QUrl base = catalogApiUrl();
    if (!base.isValid() || base.host().isEmpty())
        return;
    base.setQuery(QString());
    base.setFragment(QString());
    const auto openStatusPage = [this, base] {
        QUrl fallback = base;
        fallback.setPath(QStringLiteral("/status"));
        QDesktopServices::openUrl(fallback);
    };
    QUrl url = base;
    url.setPath(QStringLiteral("/api/accounts/admin-session"));
    const QString token = accountSessionTokenForUrl(url);
    if (!m_networkAccess || token.isEmpty()) {
        openStatusPage();
        return;
    }
    if (m_adminConsoleUrlInFlight)
        return;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("authorization",
                         QByteArrayLiteral("Bearer ") + token.toUtf8());
    request.setTransferTimeout(8000);
    m_adminConsoleUrlInFlight = true;
    QNetworkReply *reply =
        m_networkAccess->post(request, QByteArrayLiteral("{}"));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, base, openStatusPage] {
        m_adminConsoleUrlInFlight = false;
        const QByteArray body = reply->readAll();
        const bool transportOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        QString adminUrl;
        if (transportOk) {
            const QJsonObject payload =
                QJsonDocument::fromJson(body).object();
            if (payload.value(QStringLiteral("ok")).toBool())
                adminUrl =
                    payload.value(QStringLiteral("adminUrl")).toString().trimmed();
        }
        if (!adminUrl.startsWith(QLatin1Char('/'))) {
            openStatusPage();
            return;
        }
        const QUrl relative(adminUrl);
        QUrl target = base;
        target.setPath(relative.path());
        QUrlQuery query(relative.query());
        query.removeAllQueryItems(QStringLiteral("table"));
        query.addQueryItem(QStringLiteral("table"), QStringLiteral("error_log"));
        target.setQuery(query);
        QDesktopServices::openUrl(target);
    });
}

void MainWindow::refreshFooterWebsiteStatus()
{
    if (!m_networkAccess || !m_logActivityLights ||
        m_footerWebsiteStatusInFlight || !m_statusPagePackageInstalled)
        return;

    QUrl url = catalogApiUrl();
    if (!url.isValid() || url.host().isEmpty())
        return;
    url.setPath(QStringLiteral("/api/status"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("view"), QStringLiteral("world"));
    url.setQuery(query);
    url.setFragment(QString());

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("accept", "application/json");
    request.setTransferTimeout(8000);
    m_footerWebsiteStatusInFlight = true;
    m_footerStatusReadMs = QDateTime::currentMSecsSinceEpoch();
    publishFooterWebsiteStatuses();
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_footerWebsiteStatusInFlight = false;
        const QByteArray body = reply->readAll();
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool transportOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!transportOk && httpStatus <= 0) {
            publishFooterWebsiteStatuses();
            return;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(body, &error);
        if (transportOk && error.error == QJsonParseError::NoError &&
            document.isObject() &&
            applyFooterWebsiteStatusPayload(document.object()))
            return;
        applyFooterWebsiteStatusFailure(httpStatus);
    });
}

void MainWindow::refreshFooterStatusOnActivation()
{
    bool allOperational = !m_footerRelayStatuses.isEmpty();
    for (const FooterStatusRow &row : m_footerRelayStatuses) {
        if (statusSystemShown(row.id) &&
            row.status != QLatin1String("operational")) {
            allOperational = false;
            break;
        }
    }
    for (const FooterStatusRow &row : m_footerDesktopStatuses) {
        if (row.local && statusSystemShown(row.id) &&
            row.status != QLatin1String("operational")) {
            allOperational = false;
            break;
        }
    }
    for (const FooterStatusRow &row : m_footerCustomStatuses) {
        if (row.status != QLatin1String("operational")) {
            allOperational = false;
            break;
        }
    }
    qint64 healthyFloorMs = kFooterStatusHealthyFloorMs;
    if (m_packageRegistry && m_packageRegistry->isReady()) {
        const int minutes =
            m_packageRegistry
                ->declaredSetting(kStatusPageId,
                                  QStringLiteral("refreshMinutes"))
                .toString()
                .toInt();
        if (minutes > 0)
            healthyFloorMs = qint64(qBound(2, minutes, 30)) * 60 * 1000;
    }
    const qint64 floorMs = allOperational ? healthyFloorMs
                                          : kFooterStatusUnhealthyFloorMs;
    if (m_footerStatusReadMs > 0 &&
        QDateTime::currentMSecsSinceEpoch() - m_footerStatusReadMs < floorMs)
        return;
    refreshFooterWebsiteStatus();
    refreshDesktopWebsiteProbes();
    probeRelayLatency();
}

void MainWindow::applyFooterWebsiteStatusFailure(int httpStatus)
{
    if (m_footerRelayStatuses.isEmpty()) {
        publishFooterWebsiteStatuses();
        return;
    }
    const QString reason =
        httpStatus > 0
            ? QStringLiteral(
                  "The status API answered HTTP %1 instead of a status "
                  "payload, so this data is stale.")
                  .arg(httpStatus)
            : QStringLiteral("The status API did not return a usable "
                             "payload, so this data is stale.");
    for (FooterStatusRow &row : m_footerRelayStatuses) {
        row.status = QStringLiteral("unknown");
        row.reason = reason;
    }
    publishFooterWebsiteStatuses();
}

void MainWindow::refreshDesktopWebsiteProbes()
{
    // These probes download real page HTML through the redirect chain purely
    // to grade the footer status dots, so nothing calls this on a timer: only
    // launch, a recheck and an activation past the floor above.
    for (const DesktopEdgeProbe &probe : desktopEdgeProbes())
        probeDesktopWebsite(QString::fromLatin1(probe.id));
    const QList<StatusCustomMonitor> monitors = m_statusCustomMonitors;
    for (const StatusCustomMonitor &monitor : monitors)
        probeCustomStatusMonitor(monitor.id);
}

void MainWindow::probeCustomStatusMonitor(const QString &monitorId)
{
    const QString rowId = kCustomMonitorPrefix + monitorId;
    QUrl url;
    for (const StatusCustomMonitor &monitor :
         std::as_const(m_statusCustomMonitors)) {
        if (monitor.id == monitorId)
            url = QUrl(monitor.url);
    }
    if (!url.isValid() || url.host().isEmpty() || !m_networkAccess ||
        !m_logActivityLights || !m_statusPagePackageInstalled ||
        m_desktopProbesInFlight.contains(rowId))
        return;

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QVariant::fromValue(
                             QNetworkRequest::NoLessSafeRedirectPolicy));
    request.setMaximumRedirectsAllowed(5);
    request.setRawHeader("cache-control", "no-cache");
    request.setTransferTimeout(kCustomMonitorTimeoutMs);
    m_desktopProbesInFlight.insert(rowId);
    publishFooterWebsiteStatuses();
    QElapsedTimer sent;
    sent.start();
    auto answered = std::make_shared<int>(0);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, answered] {
        const int code =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code <= 0 || *answered > 0 ||
            (code >= 300 && code < 400 &&
             reply->attribute(QNetworkRequest::RedirectionTargetAttribute)
                 .isValid()))
            return;
        *answered = code;
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, rowId, monitorId, answered, sent] {
        m_desktopProbesInFlight.remove(rowId);
        if (BackoffNetworkAccessManager::isBackoffSuppressed(reply) ||
            BackoffNetworkAccessManager::isOfflineBlocked(reply)) {
            reply->deleteLater();
            publishFooterWebsiteStatuses();
            return;
        }
        int code = *answered;
        if (code <= 0)
            code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                       .toInt();
        QString transportError =
            code <= 0 && reply->error() != QNetworkReply::NoError
                ? reply->errorString()
                : QString();
        if (!transportError.isEmpty() &&
            reply->error() == QNetworkReply::OperationCanceledError &&
            sent.elapsed() >= kCustomMonitorTimeoutMs - 250) {
            transportError = QStringLiteral("No answer within %1 s")
                                 .arg(kCustomMonitorTimeoutMs / 1000);
        }
        reply->deleteLater();
        if (m_closingDown)
            return;
        QString name;
        QString address;
        for (const StatusCustomMonitor &monitor :
             std::as_const(m_statusCustomMonitors)) {
            if (monitor.id == monitorId) {
                name = monitor.name;
                address = monitor.url;
            }
        }
        if (name.isEmpty()) {
            publishFooterWebsiteStatuses();
            return;
        }
        FooterStatusRow row;
        row.id = rowId;
        row.label = name;
        row.local = true;
        row.minuteTs = QDateTime::currentMSecsSinceEpoch();
        if (code > 0 && code < 400) {
            row.status = QStringLiteral("operational");
            row.reason = QStringLiteral("%1 answered HTTP %2 from this desktop.")
                             .arg(address)
                             .arg(code);
        } else if (code >= 400) {
            row.status = QStringLiteral("down");
            row.reason = QStringLiteral("%1 returned HTTP %2 to this desktop.")
                             .arg(address)
                             .arg(code);
        } else if (m_offlineMode || osReportsOffline()) {
            row.status = QStringLiteral("unknown");
            row.reason = QStringLiteral("This desktop is offline, so %1 was "
                                        "not checked from here.")
                             .arg(address);
        } else {
            row.status = QStringLiteral("down");
            row.reason = QStringLiteral("%1 could not be reached from this "
                                        "desktop: %2")
                             .arg(address, transportError);
        }
        m_footerCustomStatuses.removeIf([&rowId](const FooterStatusRow &old) {
            return old.id == rowId;
        });
        m_footerCustomStatuses.append(row);
        publishFooterWebsiteStatuses();
    });
}

void MainWindow::probeDesktopWebsite(const QString &id)
{
    const DesktopEdgeProbe *probe = desktopEdgeProbe(id);
    if (!probe || !m_networkAccess || !m_logActivityLights ||
        m_desktopProbesInFlight.contains(id) || !m_statusPagePackageInstalled ||
        !statusSystemShown(id))
        return;

    QUrl url = catalogApiUrl();
    if (!url.isValid() || url.host().isEmpty())
        return;
    url.setPath(QString::fromLatin1(probe->path));
    url.setQuery(QString());
    url.setFragment(QString());

    // The shared network manager already opened a host-wide cooldown after a
    // real 429/5xx response. A page probe during that window would never leave
    // this process, so do not turn its synthetic reply into a fresh outage
    // sample (and two false ERROR notifications) every minute. The probe
    // resumes as soon as the cooldown expires.
    const auto *backoffManager =
        qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
    if (backoffManager && backoffManager->hostInCooldown(url.host())) {
        publishFooterWebsiteStatuses();
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QVariant::fromValue(
                             QNetworkRequest::NoLessSafeRedirectPolicy));
    request.setMaximumRedirectsAllowed(3);
    request.setRawHeader("accept", "text/html");
    request.setRawHeader("cache-control", "no-cache");
    request.setRawHeader("range", "bytes=0-8191");
    request.setTransferTimeout(kDesktopProbeTimeoutMs);
    m_desktopProbesInFlight.insert(id);
    publishFooterWebsiteStatuses();
    QElapsedTimer sent;
    sent.start();
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, url, sent] {
        m_desktopProbesInFlight.remove(id);
        // A different request can open the host cooldown between the check
        // above and get(). This marked reply is local and contains no
        // observation about the site's availability, so preserve the last
        // real probe instead of grading it as a transport outage. The same
        // holds for a request offline mode refused to send: "ForkMesh is in
        // offline mode" used to be reported as the site being down.
        if (BackoffNetworkAccessManager::isBackoffSuppressed(reply) ||
            BackoffNetworkAccessManager::isOfflineBlocked(reply)) {
            reply->deleteLater();
            publishFooterWebsiteStatuses();
            return;
        }
        const QByteArray body = reply->readAll();
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString transportError =
            (httpStatus <= 0 && reply->error() != QNetworkReply::NoError)
                ? reply->errorString()
                : QString();
        // A cancel that arrives once the whole transfer timeout has passed is
        // Qt's own timeout, not this desktop withdrawing the check: nothing
        // answered, which is worth a verdict.
        if (!transportError.isEmpty() && !m_closingDown &&
            reply->error() == QNetworkReply::OperationCanceledError &&
            sent.elapsed() >= kDesktopProbeTimeoutMs - 250) {
            transportError = QStringLiteral("No answer within %1 s")
                                 .arg(kDesktopProbeTimeoutMs / 1000);
        }
        reply->deleteLater();
        const DesktopEdgeProbe *probe = desktopEdgeProbe(id);
        if (probe && httpStatus <= 0 && !m_closingDown &&
            gradeDesktopEdgeProbe(*probe, url.host(), httpStatus, body,
                                  transportError, osReportsOffline(),
                                  m_offlineMode)
                    .status == QLatin1String("down")) {
            crossCheckDesktopWebsiteProbe(id, url, transportError);
            return;
        }
        applyDesktopWebsiteProbe(id, httpStatus, body, transportError);
    });
}

void MainWindow::crossCheckDesktopWebsiteProbe(const QString &id,
                                               const QUrl &url,
                                               const QString &transportError)
{
    // Before calling the site down, ask a different program. A per-program
    // firewall can stop this executable -- DNS included -- while the rest of
    // the computer reaches the site fine, and that read exactly like an
    // outage: red dots and "is down" pings for hours on 2026-09-21, after the
    // checkout moved and OpenSnitch denied the binary's new path (its default
    // for an unanswered prompt is deny until restart). curl ships with Linux,
    // macOS and Windows 10+; without it the check grades as it always did.
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curl.isEmpty()) {
        applyDesktopWebsiteProbe(id, 0, QByteArray(), transportError);
        return;
    }
    auto *process = new QProcess(this);
    m_desktopProbesInFlight.insert(id);
    publishFooterWebsiteStatuses();
    auto settled = std::make_shared<bool>(false);
    auto settle = [this, process, id, transportError, settled](bool answered) {
        if (*settled)
            return;
        *settled = true;
        m_desktopProbesInFlight.remove(id);
        process->deleteLater();
        applyDesktopWebsiteProbe(id, 0, QByteArray(), transportError, answered);
    };
    // Any HTTP status at all means the site answered this computer; curl
    // prints 000 when it never got one.
    connect(process, &QProcess::finished, this,
            [process, settle](int, QProcess::ExitStatus status) {
                const int code =
                    QString::fromLatin1(process->readAllStandardOutput())
                        .trimmed()
                        .toInt();
                settle(status == QProcess::NormalExit && code >= 100 &&
                       code < 600);
            });
    connect(process, &QProcess::errorOccurred, this,
            [settle](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    settle(false);
            });
    process->start(curl, {QStringLiteral("--silent"),
                          QStringLiteral("--output"), QProcess::nullDevice(),
                          QStringLiteral("--write-out"),
                          QStringLiteral("%{http_code}"),
                          QStringLiteral("--max-time"),
                          QString::number(kDesktopProbeTimeoutMs / 1000),
                          QStringLiteral("--range"), QStringLiteral("0-0"),
                          QStringLiteral("--header"),
                          QStringLiteral("cache-control: no-cache"),
                          url.toString()});
}

void MainWindow::applyDesktopWebsiteProbe(const QString &id, int httpStatus,
                                          const QByteArray &body,
                                          const QString &transportError,
                                          bool reachableByOtherPrograms)
{
    const DesktopEdgeProbe *probe = desktopEdgeProbe(id);
    if (!probe)
        return;
    const QString host = catalogApiUrl().host();
    DesktopEdgeVerdict sample = gradeDesktopEdgeProbe(
        *probe, host, httpStatus, body, transportError, osReportsOffline(),
        m_offlineMode);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QList<DesktopProbeSample> &history = m_desktopProbeHistory[id];
    const bool blockedHere = reachableByOtherPrograms && httpStatus <= 0;
    QString blockingFirewall;
    if (blockedHere) {
        // The site answered another program on this computer, so this is no
        // outage: something here is stopping ForkMesh itself. The failures
        // this desktop saw before were most likely the same block, so they
        // stop holding the dot red as well.
        blockingFirewall = runningAppFirewallName();
        sample = {QStringLiteral("unknown"),
                  QStringLiteral("%1 answers other programs on this computer, "
                                 "but %2 is stopping ForkMesh from reaching "
                                 "it (%3).")
                      .arg(host.isEmpty() ? QStringLiteral("The site") : host,
                           blockingFirewall.isEmpty()
                               ? QStringLiteral("a firewall or security "
                                                "program")
                               : blockingFirewall,
                           transportError)};
        history.clear();
    }
    if (sample.status != QLatin1String("unknown"))
        history.append({now, sample.status});
    while (!history.isEmpty() &&
           (history.size() > kDesktopProbeHistoryMax ||
            now - history.first().ts > kDesktopProbeHistoryMs))
        history.removeFirst();
    int downs = 0;
    qint64 lastDownTs = 0;
    for (const DesktopProbeSample &past : history) {
        if (past.status != QLatin1String("down"))
            continue;
        ++downs;
        lastDownTs = past.ts;
    }
    const DesktopEdgeVerdict verdict = mergeDesktopEdgeHistory(
        sample, QString::fromLatin1(probe->what), downs, history.size(),
        lastDownTs);

    FooterStatusRow row;
    row.id = id;
    row.label = QString::fromLatin1(probe->label);
    row.status = verdict.status;
    row.sampleStatus = sample.status;
    row.reason = verdict.reason;
    row.minuteTs = now;
    row.local = true;

    bool replaced = false;
    for (FooterStatusRow &existing : m_footerDesktopStatuses) {
        if (existing.id == id) {
            existing = row;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        m_footerDesktopStatuses.append(row);
        std::stable_sort(m_footerDesktopStatuses.begin(),
                         m_footerDesktopStatuses.end(),
                         [](const FooterStatusRow &left,
                            const FooterStatusRow &right) {
                             return desktopEdgeProbeRank(left.id) <
                                    desktopEdgeProbeRank(right.id);
                         });
    }
    publishFooterWebsiteStatuses();
    alertOnDesktopEdgeOutage(row);
    if (blockedHere)
        alertOnLocalFirewallBlock(host, blockingFirewall);
}

void MainWindow::alertOnLocalFirewallBlock(const QString &host,
                                           const QString &firewall)
{
    // Both page checks land here in the same round, and activation re-runs
    // them every couple of minutes while a dot is not green: one ping says
    // it, a stream of them would bury it.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_localFirewallAlertMs > 0 &&
        now - m_localFirewallAlertMs < kLocalFirewallAlertGapMs)
        return;
    m_localFirewallAlertMs = now;
    const QString program =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString title =
        firewall.isEmpty()
            ? QStringLiteral("A firewall on this computer is blocking ForkMesh")
            : QStringLiteral("%1 is blocking ForkMesh").arg(firewall);
    const QString where =
        firewall.isEmpty()
            ? QStringLiteral("in the firewall or security program that is "
                             "stopping it")
            : QStringLiteral("in %1: answer its prompt with Allow, or change "
                             "its rule for this program")
                  .arg(firewall);
    const QString body =
        QStringLiteral("%1 is up and answers other programs here, but not "
                       "ForkMesh, so sync, chat and update checks are "
                       "stopped. Allow %2 %3. A firewall that matches "
                       "programs by path treats a rebuilt or moved ForkMesh "
                       "as a new program.")
            .arg(host.isEmpty() ? QStringLiteral("The site") : host, program,
                 where);
    // Kept on this machine: it is this computer's setup, not a fault for the
    // relay's error log.
    addNotification(title, body, /*warning=*/true, NotificationLink(),
                    forkmesh::kLocalOperationNoticeKind, QString());
}

void MainWindow::alertOnDesktopEdgeOutage(const FooterStatusRow &row)
{
    if (row.sampleStatus != QLatin1String("down"))
        return;
    const DesktopEdgeProbe *probe = desktopEdgeProbe(row.id);
    if (!probe)
        return;
    const QString host = catalogApiUrl().host();
    const QString title =
        host.isEmpty()
            ? QStringLiteral("%1 is down").arg(QString::fromLatin1(probe->what))
            : QStringLiteral("%1 on %2 is down")
                  .arg(QString::fromLatin1(probe->what), host);
    const QString body =
        row.reason.isEmpty()
            ? QStringLiteral("This desktop could not load the page.")
            : row.reason;
    addNotification(title, body, /*warning=*/true);
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testApplyDesktopWebsiteProbe(const QString &id,
                                                 int httpStatus,
                                                 const QByteArray &body,
                                                 const QString &transportError,
                                                 bool reachableByOtherPrograms)
{
    applyDesktopWebsiteProbe(id, httpStatus, body, transportError,
                             reachableByOtherPrograms);
    for (const FooterStatusRow &row : m_footerDesktopStatuses) {
        if (row.id == id)
            return row.sampleStatus;
    }
    return QString();
}

QString MainWindow::testDesktopWebsiteRowStatus(const QString &id) const
{
    for (const FooterStatusRow &row : m_footerDesktopStatuses) {
        if (row.id == id)
            return row.status;
    }
    return QString();
}
#endif

QString MainWindow::backgroundTaskWord(const QString &kind)
{
    QString word;
    for (const QChar ch : kind.simplified()) {
        if (ch.isSpace())
            break;
        if (ch.isLetterOrNumber())
            word.append(ch.toLower());
    }
    if (word.isEmpty())
        word = QStringLiteral("work");
    return word.left(10);
}

quint64 MainWindow::beginBackgroundTask(const QString &kind,
                                        const QString &detail,
                                        const forkmesh::LogSource &source)
{
    return forkmesh::BackgroundActivity::begin(
        kind, detail, forkmesh::ActionTelemetry::Execution::Async, source);
}

void MainWindow::finishBackgroundTask(quint64 id, bool success,
                                      const QString &detail,
                                      const QString &failureKind)
{
    forkmesh::BackgroundActivity::end(
        id, success ? QStringLiteral("succeeded") : QStringLiteral("failed"));
    if (!detail.trimmed().isEmpty())
        logSystem(QStringLiteral("Background: %1").arg(detail.trimmed()));
    if (!success && !detail.trimmed().isEmpty())
        flashMessage(detail.trimmed(), true, QString(), failureKind);
}

namespace {
// The outcome line lists every routine a tally's runs ran for, so a `git
// status` the rail timer and a window activation both asked for says so. A
// handful is plenty: past that the line stops being readable, and the action
// journal has every ticket anyway.
constexpr int kBackgroundRoutinesShown = 4;

void noteBackgroundRoutine(QStringList *routines, const QString &routine)
{
    if (!routines || routine.isEmpty() || routines->contains(routine) ||
        routines->size() >= kBackgroundRoutinesShown)
        return;
    routines->append(routine);
}
} // namespace

void MainWindow::recordBackgroundOutcome(const QString &word, qint64 elapsedMs,
                                         const QString &detail, qint64 now,
                                         bool backgrounded,
                                         const forkmesh::LogSource &source,
                                         const BackgroundTaskContext &context)
{
    QHash<QString, BackgroundOutcomeTally> &bucket =
        backgrounded ? m_backgroundTaskDone : m_backgroundTaskUiBlocking;
    BackgroundOutcomeTally &tally = bucket[word];
    if (tally.runs == 0)
        tally.firstAt = now;
    ++tally.runs;
    tally.longestMs = qMax(tally.longestMs, elapsedMs);
    const QString note = detail.trimmed();
    if (!note.isEmpty()) {
        tally.detail = note;
        tally.source = source; // the call site that produced this note
    }
    for (const QString &routine : context.routines)
        noteBackgroundRoutine(&tally.routines, routine);
    if (!context.workingDirectory.isEmpty())
        tally.workingDirectory = context.workingDirectory;
    tally.longestQueuedMs =
        qMax(tally.longestQueuedMs, context.longestQueuedMs);
    tally.failures += context.failures;
    if (!context.lastFailure.isEmpty())
        tally.lastFailure = context.lastFailure;
}

void MainWindow::describeBackgroundWorkingDirectory(
    const QString &dir, forkmesh::BackgroundOutcomeContext *context) const
{
    if (!context || dir.trimmed().isEmpty())
        return;
    const QString clean = QDir::cleanPath(dir);
    const auto sameOrInside = [&clean](const QString &root) {
        if (root.trimmed().isEmpty())
            return false;
        const QString cleanRoot = QDir::cleanPath(root);
        return clean == cleanRoot ||
               clean.startsWith(cleanRoot + QLatin1Char('/'));
    };
    // The repository whose checkout holds the directory. An agent worktree
    // lives under <checkout>/.worktrees/, so it resolves to its repository too.
    for (const RepositoryRecord &repo : m_repositories) {
        if (sameOrInside(repo.localPath)) {
            context->repo = repo.owner + QLatin1Char('/') + repo.name;
            break;
        }
    }
    // The agent whose worktree the directory is: the live path a stream
    // recorded, the one a reload resolved, or — for a session that is neither
    // — the directory name ForkMesh derives from its id and title.
    const QString leaf = clean.section(QLatin1Char('/'), -1);
    const bool underWorktreeRoot =
        clean.contains(QLatin1String("/.worktrees/")) ||
        clean.contains(QLatin1String("/forkmesh-worktrees/"));
    for (const AgentSession &session : m_agentSessions) {
        const QString live = m_streamWorktree.value(session.id);
        const QString cached = m_sessionWorkdirCache.value(session.id);
        bool hit = (!live.isEmpty() && QDir::cleanPath(live) == clean) ||
                   (!cached.isEmpty() && QDir::cleanPath(cached) == clean);
        if (!hit && underWorktreeRoot) {
            const QString name =
                session.worktreeName.isEmpty()
                    ? forkmesh::agentwt::dirName(session.id, session.issueTitle)
                    : session.worktreeName;
            hit = !name.isEmpty() && name == leaf;
        }
        if (!hit)
            continue;
        context->agent = QStringLiteral("#%1 %2")
                             .arg(session.id)
                             .arg(session.status.isEmpty()
                                      ? QStringLiteral("unknown")
                                      : session.status);
        break;
    }
    if (context->repo.isEmpty() && context->agent.isEmpty()) {
        QString shown = clean;
        const QString home = QDir::cleanPath(QDir::homePath());
        if (shown == home)
            shown = QStringLiteral("~");
        else if (shown.startsWith(home + QLatin1Char('/')))
            shown = QStringLiteral("~") + shown.mid(home.size());
        context->path = shown;
    }
}

void MainWindow::flushBackgroundOutcomes(bool force)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (QHash<QString, BackgroundOutcomeTally> *bucket :
         {&m_backgroundTaskDone, &m_backgroundTaskUiBlocking}) {
        const bool backgrounded = bucket == &m_backgroundTaskDone;
        for (auto it = bucket->begin(); it != bucket->end();) {
            if (!force && now - it->firstAt < kBackgroundTaskFastFlushMs) {
                ++it;
                continue;
            }
            // Attributed to the call site the ticket captured, not to this
            // loop: the whole point of the link is to open the code that keeps
            // asking for the work.
            forkmesh::BackgroundOutcomeContext context;
            context.routines = it->routines;
            context.function = forkmesh::logSourceFunctionName(it->source);
            describeBackgroundWorkingDirectory(it->workingDirectory, &context);
            context.failures = it->failures;
            context.lastFailure = it->lastFailure;
            context.longestQueuedMs = it->longestQueuedMs;
            const QString outcome = forkmesh::backgroundOutcomeLine(
                it.key(), it->runs, it->longestMs, it->detail, backgrounded,
                context);
            if (it->source.isValid())
                logSystemFrom(outcome,
                              forkmesh::logSourceRelativePath(it->source.file),
                              it->source.line);
            else
                logSystem(outcome);
            it = bucket->erase(it);
        }
    }
}

void MainWindow::noteBackgroundActivity(const forkmesh::BackgroundTicket &ticket,
                                        bool backgrounded, bool started)
{
    noteAgentActivityTask(ticket, started);
    if (!m_statusBackgroundLayout)
        return;
    if (started) {
        const QString word = backgroundTaskWord(ticket.kind);
        m_backgroundTaskWords.insert(ticket.id, word);
        const qint64 startedAt = QDateTime::currentMSecsSinceEpoch();
        const int count = m_backgroundTaskCounts.value(word) + 1;
        m_backgroundTaskCounts.insert(word, count);
        if (count == 1)
            m_backgroundTaskSince.insert(word, startedAt);
        const QString note = forkmesh::backgroundDetailNote(ticket.detail);
        if (!backgrounded) {
            m_backgroundTaskBlockingStarts.insert(ticket.id, startedAt);
            BackgroundBlockingSpan &span = m_backgroundTaskBlocking[word];
            if (!note.isEmpty()) {
                span.detail = note;
                span.source = ticket.source;
            }
            noteBackgroundRoutine(&span.context.routines, ticket.routine);
            if (!ticket.workingDirectory.isEmpty())
                span.context.workingDirectory = ticket.workingDirectory;
            span.context.longestQueuedMs =
                qMax(span.context.longestQueuedMs, ticket.queuedMs);
        } else {
            m_backgroundTaskBackgroundedRuns[word] += 1;
            if (!note.isEmpty()) {
                m_backgroundTaskDetails.insert(word, note);
                m_backgroundTaskOrigins.insert(word, ticket.source);
            }
            BackgroundTaskContext &context = m_backgroundTaskContexts[word];
            noteBackgroundRoutine(&context.routines, ticket.routine);
            if (!ticket.workingDirectory.isEmpty())
                context.workingDirectory = ticket.workingDirectory;
            context.longestQueuedMs =
                qMax(context.longestQueuedMs, ticket.queuedMs);
        }
    } else {
        const QString word = m_backgroundTaskWords.take(ticket.id);
        if (word.isEmpty())
            return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const QString failure =
            forkmesh::backgroundFailureText(ticket.outcome);
        const auto blockingStart = m_backgroundTaskBlockingStarts.constFind(ticket.id);
        if (blockingStart != m_backgroundTaskBlockingStarts.constEnd()) {
            BackgroundBlockingSpan &span = m_backgroundTaskBlocking[word];
            span.longestMs = qMax(span.longestMs, now - blockingStart.value());
            if (!failure.isEmpty()) {
                ++span.context.failures;
                span.context.lastFailure = failure;
            }
            m_backgroundTaskBlockingStarts.erase(blockingStart);
        } else if (!failure.isEmpty()) {
            BackgroundTaskContext &context = m_backgroundTaskContexts[word];
            ++context.failures;
            context.lastFailure = failure;
        }
        const int count = m_backgroundTaskCounts.value(word) - 1;
        if (count > 0) {
            m_backgroundTaskCounts.insert(word, count);
        } else {
            const auto span = m_backgroundTaskBlocking.constFind(word);
            if (span != m_backgroundTaskBlocking.constEnd())
                recordBackgroundOutcome(word, span->longestMs, span->detail,
                                        now, false, span->source,
                                        span->context);
            if (m_backgroundTaskBackgroundedRuns.value(word) > 0)
                recordBackgroundOutcome(
                    word, now - m_backgroundTaskSince.value(word, now),
                    m_backgroundTaskDetails.value(word), now, true,
                    m_backgroundTaskOrigins.value(word),
                    m_backgroundTaskContexts.value(word));
            m_backgroundTaskCounts.remove(word);
            m_backgroundTaskSince.remove(word);
            m_backgroundTaskDetails.remove(word);
            m_backgroundTaskOrigins.remove(word);
            m_backgroundTaskBlocking.remove(word);
            m_backgroundTaskBackgroundedRuns.remove(word);
            m_backgroundTaskContexts.remove(word);
        }
    }
    if (!m_backgroundTaskSpinTimer) {
        m_backgroundTaskSpinTimer = new QTimer(this);
        m_backgroundTaskSpinTimer->setInterval(90);
        connect(m_backgroundTaskSpinTimer, &QTimer::timeout, this,
                &MainWindow::tickBackgroundQueue);
    }
    if (!m_backgroundTaskSpinTimer->isActive()) {
        m_backgroundTaskIdleTicks = 0;
        m_backgroundTaskSpinTimer->start();
    }
}

void MainWindow::tickBackgroundQueue()
{
    if (!m_statusBackgroundLayout)
        return;
    m_backgroundTaskSpinAngle =
        std::fmod(m_backgroundTaskSpinAngle + 9.0, 360.0);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    const QStringList shown = m_backgroundTaskChips.keys();
    for (const QString &word : shown) {
        if (m_backgroundTaskCounts.contains(word))
            continue;
        if (QWidget *chip = m_backgroundTaskChips.take(word)) {
            m_statusBackgroundLayout->removeWidget(chip);
            chip->hide();
            chip->deleteLater();
        }
    }

    for (auto it = m_backgroundTaskCounts.constBegin();
         it != m_backgroundTaskCounts.constEnd(); ++it) {
        const QString &word = it.key();
        if (now - m_backgroundTaskSince.value(word, now) <
            kBackgroundTaskShowAfterMs)
            continue;
        forkmesh::ui::BackgroundTaskChip *chip =
            m_backgroundTaskChips.value(word);
        if (!chip) {
            chip = new forkmesh::ui::BackgroundTaskChip(word,
                                                        m_statusBackgroundHost);
            chip->setObjectName("statusBackgroundTaskChip");
            int slot = 0;
            for (auto chipIt = m_backgroundTaskChips.constBegin();
                 chipIt != m_backgroundTaskChips.constEnd(); ++chipIt)
                if (chipIt.key() < word)
                    ++slot;
            // The leading label stays before all sorted task indicators.
            m_statusBackgroundLayout->insertWidget(slot + 1, chip);
            m_backgroundTaskChips.insert(word, chip);
        }
        const int count = it.value();
        chip->setCount(count);
        if (chip->isVisible() && isVisible() && !isMinimized())
            chip->setAngle(m_backgroundTaskSpinAngle);
        const auto blocking = m_backgroundTaskBlocking.constFind(word);
        QString note = m_backgroundTaskDetails.value(word);
        if (note.isEmpty() && blocking != m_backgroundTaskBlocking.constEnd())
            note = blocking->detail;
        QStringList routines = m_backgroundTaskContexts.value(word).routines;
        if (blocking != m_backgroundTaskBlocking.constEnd())
            for (const QString &routine : blocking->context.routines)
                noteBackgroundRoutine(&routines, routine);
        if (!routines.isEmpty())
            note += (note.isEmpty() ? QString() : QStringLiteral("\n")) +
                    QStringLiteral("for ") +
                    routines.join(QStringLiteral(", "));
        const QString tip = count > 1 ? QStringLiteral("%1 %2%3\n%4")
                                            .arg(word)
                                            .arg(QChar(0x00D7))
                                            .arg(count)
                                            .arg(note)
                                            .trimmed()
                                      : QStringLiteral("%1\n%2")
                                            .arg(word, note)
                                            .trimmed();
        if (chip->toolTip() != tip)
            chip->setToolTip(tip);
    }

    const int visible = m_backgroundTaskChips.size();
    if (m_statusBackgroundLabel)
        m_statusBackgroundLabel->setVisible(visible > 0);

    if (m_backgroundTaskCounts.isEmpty() && visible == 0) {
        const bool standingDown =
            ++m_backgroundTaskIdleTicks >= kBackgroundTaskIdleTicksBeforeStop;
        flushBackgroundOutcomes(standingDown);
        if (standingDown && m_backgroundTaskSpinTimer) {
            m_backgroundTaskSpinTimer->stop();
            m_backgroundTaskIdleTicks = 0;
        }
    } else {
        m_backgroundTaskIdleTicks = 0;
        flushBackgroundOutcomes(false);
    }
}

void MainWindow::openQuickAddSlashActions()
{
    if (!m_quickAddSlashButton || !m_issueQuickAdd)
        return;
    if (!m_slashActionsPopup) {
        m_slashActionsPopup = new QFrame(this);
        m_slashActionsPopup->setObjectName("slashActionsPopup");
        m_slashActionsPopup->setWindowFlags(Qt::Popup);
        m_slashActionsPopup->setFixedWidth(340);

        auto *popupLayout = new QVBoxLayout(m_slashActionsPopup);
        popupLayout->setContentsMargins(0, 0, 0, 0);
        popupLayout->setSpacing(0);

        m_slashActionsFilter = new QLineEdit;
        m_slashActionsFilter->setObjectName("slashActionsFilter");
        m_slashActionsFilter->setPlaceholderText("Filter actions\xE2\x80\xA6");
        m_slashActionsFilter->installEventFilter(this);
        connect(m_slashActionsFilter, &QLineEdit::textChanged, this,
                &MainWindow::populateSlashActionsList);
        popupLayout->addWidget(m_slashActionsFilter);

        m_slashActionsListHost = new QWidget;
        m_slashActionsListLayout = new QVBoxLayout(m_slashActionsListHost);
        m_slashActionsListLayout->setContentsMargins(0, 6, 0, 6);
        m_slashActionsListLayout->setSpacing(0);

        m_slashActionsScroll = new QScrollArea;
        m_slashActionsScroll->setObjectName("slashActionsScroll");
        m_slashActionsScroll->setWidget(m_slashActionsListHost);
        m_slashActionsScroll->setWidgetResizable(true);
        m_slashActionsScroll->setFrameShape(QFrame::NoFrame);
        m_slashActionsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_slashActionsScroll->setFixedHeight(360);
        popupLayout->addWidget(m_slashActionsScroll);
    }

    {
        const QSignalBlocker block(m_slashActionsFilter);
        m_slashActionsFilter->clear();
    }
    populateSlashActionsList();
    // Claude exposes its command catalog through its control protocol. Codex
    // commands are handled by app-server and do not require launching a second
    // probe merely to open this menu.
    if (m_quickAddAgentProvider &&
        m_quickAddAgentProvider->currentData().toString() ==
            QLatin1String("claude-code"))
        refreshClaudeSlashCommands();

    m_slashActionsPopup->adjustSize();
    const QPoint above = m_quickAddSlashButton->mapToGlobal(
        QPoint(0, -m_slashActionsPopup->sizeHint().height() - 4));
    m_slashActionsPopup->move(above);
    m_slashActionsPopup->show();
    m_slashActionsFilter->setFocus();
}

void MainWindow::populateSlashActionsList()
{
    if (!m_slashActionsListLayout || !m_slashActionsListHost)
        return;
    QLayoutItem *item;
    while ((item = m_slashActionsListLayout->takeAt(0)) != nullptr) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    m_slashActionRows.clear();
    m_slashActionSelected = -1;

    const QString filter =
        m_slashActionsFilter ? m_slashActionsFilter->text().trimmed().toLower() : QString();
    auto matches = [&filter](const QString &label, const QString &extra = QString()) {
        return filter.isEmpty() || label.toLower().contains(filter) ||
               extra.toLower().contains(filter);
    };
    auto addHeader = [this](const QString &text) {
        auto *header = new QLabel(text);
        header->setObjectName("slashActionsHeader");
        m_slashActionsListLayout->addWidget(header);
    };
    auto addRow = [this](const QString &label, const QString &rightText,
                         const QString &kind, const QString &value) -> QWidget * {
        auto *row = new QFrame;
        row->setObjectName("slashActionRow");
        row->setCursor(Qt::PointingHandCursor);
        row->setProperty("slashKind", kind);
        row->setProperty("slashValue", value);
        row->installEventFilter(this);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 7, 12, 7);
        auto *title = new QLabel(label);
        title->setObjectName("slashActionRowLabel");
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        rowLayout->addWidget(title, 1);
        if (!rightText.isEmpty()) {
            auto *right = new QLabel(rightText);
            right->setObjectName("slashActionRowValue");
            right->setAttribute(Qt::WA_TransparentForMouseEvents);
            rowLayout->addWidget(right);
        }
        m_slashActionsListLayout->addWidget(row);
        m_slashActionRows.append(row);
        return row;
    };
    auto addToggleRow = [this](const QString &label, const QString &kind, bool checked) {
        auto *row = new QFrame;
        row->setObjectName("slashActionRow");
        row->setCursor(Qt::PointingHandCursor);
        row->setProperty("slashKind", kind);
        row->installEventFilter(this);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 7, 12, 7);
        auto *title = new QLabel(label);
        title->setObjectName("slashActionRowLabel");
        title->setWordWrap(true);
        title->setAttribute(Qt::WA_TransparentForMouseEvents); // see addRow above
        rowLayout->addWidget(title, 1);
        auto *toggle = new QCheckBox;
        toggle->setObjectName("slashToggle");
        toggle->setChecked(checked);
        toggle->setAttribute(Qt::WA_TransparentForMouseEvents); // the row owns the click
        toggle->setFocusPolicy(Qt::NoFocus);
        rowLayout->addWidget(toggle);
        m_slashActionsListLayout->addWidget(row);
        m_slashActionRows.append(row);
    };

    struct ContextAction { QString label; QString kind; };
    const ContextAction contextActions[] = {
        {QString::fromUtf8("Attach file\xE2\x80\xA6"), QStringLiteral("attachFile")},
        {QString::fromUtf8("Mention file from this project\xE2\x80\xA6"), QStringLiteral("mentionFile")},
        {QStringLiteral("Clear conversation"), QStringLiteral("clearConversation")},
        {QStringLiteral("Rewind"), QStringLiteral("rewind")},
        {QStringLiteral("Send issue context to agent"), QStringLiteral("sendIssueContext")},
    };
    bool anyContext = false;
    for (const ContextAction &a : contextActions)
        if (matches(a.label)) { anyContext = true; break; }
    if (anyContext) {
        addHeader(QStringLiteral("Context"));
        for (const ContextAction &a : contextActions)
            if (matches(a.label))
                addRow(a.label, QString(), a.kind, QString());
    }

    const bool codexProvider =
        m_quickAddAgentProvider &&
        agentIsCodexProvider(m_quickAddAgentProvider->currentData().toString());
    const QStringList effortLevels = agentEffortLevels();
    QStringList effortLabels;
    for (const QString &level : effortLevels)
        effortLabels << agentEffortLabel(level);
    const QString currentEffort =
        QSettings().value(kClaudeEffortSetting, QStringLiteral("high")).toString();
    int effortIdx = effortLevels.indexOf(currentEffort);
    if (effortIdx < 0) {
        effortIdx = qMax(0, effortLevels.indexOf(QStringLiteral("high")));
        if (codexProvider && effortIdx < effortLevels.size())
            QSettings().setValue(kClaudeEffortSetting,
                                 effortLevels.at(effortIdx));
    }
    const bool modelSectionMatches =
        matches(QStringLiteral("Switch model")) || matches(QStringLiteral("Effort")) ||
        (!codexProvider && matches(QStringLiteral("Thinking"))) ||
        (!codexProvider &&
         matches(QStringLiteral("Switch models when a message is flagged"))) ||
        matches(QStringLiteral("Account & usage"));
    if (modelSectionMatches) {
        addHeader(QStringLiteral("Model"));
        if (matches(QStringLiteral("Switch model")))
            addRow(QString::fromUtf8("Switch model\xE2\x80\xA6"),
                   m_quickAddAgentModelSelector
                       ? m_quickAddAgentModelSelector->currentText()
                       : QString(),
                   QStringLiteral("switchModel"), QString());
        if (matches(QStringLiteral("Effort"))) {
            auto *row = new QFrame;
            row->setObjectName("slashActionRow");
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(12, 7, 12, 7);
            auto *title = new QLabel(
                QStringLiteral("Effort (%1)").arg(effortLabels.value(effortIdx)));
            title->setObjectName("slashActionRowLabel");
            rowLayout->addWidget(title, 1);
            auto *dotsLayout = new QHBoxLayout;
            dotsLayout->setSpacing(4);
            for (int i = 0; i < effortLevels.size(); ++i) {
                auto *dot = new QToolButton;
                dot->setObjectName("slashEffortDot");
                dot->setCheckable(true);
                dot->setChecked(i == effortIdx);
                dot->setFixedSize(10, 10);
                dot->setCursor(Qt::PointingHandCursor);
                dot->setProperty("slashKind", QStringLiteral("effortLevel"));
                dot->setProperty("slashValue", effortLevels.at(i));
                dot->installEventFilter(this);
                dotsLayout->addWidget(dot);
                m_slashActionRows.append(dot); // arrow-key reachable, like the rows
            }
            rowLayout->addLayout(dotsLayout);
            m_slashActionsListLayout->addWidget(row);
        }
        if (!codexProvider && matches(QStringLiteral("Thinking")))
            addToggleRow(QStringLiteral("Thinking"), QStringLiteral("toggleThinking"),
                        QSettings().value(kClaudeThinkingSetting, true).toBool());
        if (!codexProvider &&
            matches(QStringLiteral("Switch models when a message is flagged")))
            addToggleRow(QStringLiteral("Switch models when a message is flagged"),
                        QStringLiteral("toggleFallback"),
                        QSettings().value(kClaudeFallbackModelSetting, false).toBool());
        if (matches(QStringLiteral("Account & usage")))
            addRow(QString::fromUtf8("Account & usage\xE2\x80\xA6"), QString(),
                   QStringLiteral("accountUsage"), QString());
    }

    if (!codexProvider && !m_claudeSlashCommands.isEmpty()) {
        bool anyCmd = false;
        for (const ClaudeSlashCommand &c : m_claudeSlashCommands)
            if (matches(c.name, c.description)) { anyCmd = true; break; }
        if (anyCmd) {
            addHeader(QStringLiteral("Commands"));
            for (const ClaudeSlashCommand &c : m_claudeSlashCommands) {
                if (!matches(c.name, c.description))
                    continue;
                addRow(QStringLiteral("/%1").arg(c.name), QString(),
                       QStringLiteral("command"), c.name);
            }
        }
    }

    m_slashActionsListLayout->addStretch(1);
    if (!m_slashActionRows.isEmpty()) {
        m_slashActionSelected = 0;
        m_slashActionRows.first()->setProperty("slashSelected", true);
        m_slashActionRows.first()->style()->unpolish(m_slashActionRows.first());
        m_slashActionRows.first()->style()->polish(m_slashActionRows.first());
    }
}

void MainWindow::moveSlashActionsSelection(int delta)
{
    if (m_slashActionRows.isEmpty())
        return;
    if (m_slashActionSelected >= 0 && m_slashActionSelected < m_slashActionRows.size()) {
        QWidget *prev = m_slashActionRows.at(m_slashActionSelected);
        prev->setProperty("slashSelected", false);
        prev->style()->unpolish(prev);
        prev->style()->polish(prev);
    }
    int next = qBound(0, m_slashActionSelected + delta, m_slashActionRows.size() - 1);
    m_slashActionSelected = next;
    QWidget *row = m_slashActionRows.at(next);
    row->setProperty("slashSelected", true);
    row->style()->unpolish(row);
    row->style()->polish(row);
    if (m_slashActionsScroll)
        m_slashActionsScroll->ensureWidgetVisible(row);
}

void MainWindow::activateSlashActionRow(QWidget *row)
{
    if (!row)
        return;
    const QString kind = row->property("slashKind").toString();
    const QString value = row->property("slashValue").toString();
    auto closePopup = [this] { if (m_slashActionsPopup) m_slashActionsPopup->hide(); };

    if (kind == QLatin1String("attachFile")) {
        closePopup();
        attachQuickAddMedia();
    } else if (kind == QLatin1String("mentionFile")) {
        closePopup();
        mentionProjectFileInQuickAdd();
    } else if (kind == QLatin1String("clearConversation")) {
        if (m_issueQuickAdd)
            m_issueQuickAdd->clear();
        clearQuickAddImages();
        m_quickAddHistoryIndex = -1;
        closePopup();
    } else if (kind == QLatin1String("rewind")) {
        navigateQuickAddHistory(-1);
        closePopup();
    } else if (kind == QLatin1String("sendIssueContext")) {
        closePopup();
        sendIssueContextToSelectedAgent();
    } else if (kind == QLatin1String("switchModel")) {
        closePopup();
        if (m_quickAddAgentModelSelector) {
            m_quickAddAgentModelSelector->setFocus();
            m_quickAddAgentModelSelector->showPopup();
        }
    } else if (kind == QLatin1String("effortLevel")) {
        QSettings().setValue(kClaudeEffortSetting, value);
        refreshQuickAddSpeedSelector();
        populateSlashActionsList();
    } else if (kind == QLatin1String("toggleThinking")) {
        QSettings().setValue(kClaudeThinkingSetting,
                             !QSettings().value(kClaudeThinkingSetting, true).toBool());
        populateSlashActionsList();
    } else if (kind == QLatin1String("toggleFallback")) {
        QSettings().setValue(
            kClaudeFallbackModelSetting,
            !QSettings().value(kClaudeFallbackModelSetting, false).toBool());
        populateSlashActionsList();
    } else if (kind == QLatin1String("accountUsage")) {
        closePopup();
        openAgentsOverview();
    } else if (kind == QLatin1String("command")) {
        if (m_issueQuickAdd) {
            QTextCursor cursor = m_issueQuickAdd->textCursor();
            cursor.movePosition(QTextCursor::End);
            if (!m_issueQuickAdd->toPlainText().isEmpty() &&
                !m_issueQuickAdd->toPlainText().endsWith(QLatin1Char('\n')))
                cursor.insertText(QStringLiteral("\n"));
            cursor.insertText(QStringLiteral("/%1 ").arg(value));
            m_issueQuickAdd->setTextCursor(cursor);
        }
        closePopup();
        if (m_issueQuickAdd)
            m_issueQuickAdd->setFocus();
    }
}

QStringList MainWindow::agentEffortLevels() const
{
    const QString provider =
        m_quickAddAgentProvider ? m_quickAddAgentProvider->currentData().toString()
                                : QString();
    if (agentIsCodexProvider(provider) && m_quickAddClaudeModel) {
        const QString selectedModel = selectedModelComboValue(m_quickAddClaudeModel);
        const QJsonArray models =
            QJsonDocument::fromJson(
                QSettings().value(kCodexModelsCacheSetting).toByteArray())
                .array();
        for (const QJsonValue &value : models) {
            const QJsonObject model = value.toObject();
            QString id = model.value(QStringLiteral("model")).toString();
            if (id.isEmpty())
                id = model.value(QStringLiteral("id")).toString();
            if (id != selectedModel)
                continue;
            QStringList levels;
            for (const QJsonValue &effortValue :
                 model.value(QStringLiteral("supportedReasoningEfforts")).toArray()) {
                const QString level = effortValue.toObject()
                                          .value(QStringLiteral("reasoningEffort"))
                                          .toString();
                if (!level.isEmpty())
                    levels << level;
            }
            if (!levels.isEmpty())
                return levels;
            break;
        }
        return defaultAgentEffortLevels();
    }
    const QStringList probed =
        QSettings().value(kClaudeEffortLevelsCacheSetting).toStringList();
    return probed.isEmpty() ? defaultAgentEffortLevels() : probed;
}

void MainWindow::refreshCloudflareAiModels()
{
    if (m_cloudflareAiModelsFetched || !m_networkAccess)
        return;
    const QMap<QString, QString> variables = ActionStore::variables();
    const QString apiToken =
        forkmesh::control::cloudflareApiTokenFromVariables(variables);
    const QString accountId =
        forkmesh::control::cloudflareAccountIdFromVariables(variables);
    if (apiToken.isEmpty() || accountId.isEmpty())
        return;
    m_cloudflareAiModelsFetched = true;
    const QString encodedAccount = QString::fromLatin1(
        QUrl::toPercentEncoding(accountId));
    QUrl url(QStringLiteral(
        "https://api.cloudflare.com/client/v4/accounts/%1/ai/models/search")
                 .arg(encodedAccount));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("task"), QStringLiteral("text-generation"));
    query.addQueryItem(QStringLiteral("hide_experimental"), QStringLiteral("true"));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + apiToken.toUtf8());
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray payload = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!ok)
            return;
        const QJsonArray models =
            QJsonDocument::fromJson(payload).object().value(QStringLiteral("result"))
                .toArray();
        if (models.isEmpty())
            return;
        QJsonArray cached;
        for (const QJsonValue &value : models) {
            const QJsonObject model = value.toObject();
            const QString id = model.value(QStringLiteral("name")).toString().trimmed();
            if (!id.startsWith(QLatin1String("@cf/")))
                continue;
            const QString description =
                model.value(QStringLiteral("description")).toString().trimmed();
            QString label =
                model.value(QStringLiteral("display_name")).toString().trimmed();
            if (label.isEmpty())
                label = cloudflareAiModelLabel(id);
            cached.append(QJsonObject{{QStringLiteral("id"), id},
                                       {QStringLiteral("label"), label},
                                       {QStringLiteral("description"), description}});
        }
        if (cached.isEmpty())
            return;
        QSettings().setValue(
            kCloudflareAiModelsCacheSetting,
            QJsonDocument(cached).toJson(QJsonDocument::Compact));
        if (m_quickAddClaudeModel && m_quickAddAgentProvider &&
            agentIsCloudflareAiProvider(
                m_quickAddAgentProvider->currentData().toString())) {
            const QSignalBlocker block(m_quickAddClaudeModel);
            const QString picked = selectedModelComboValue(m_quickAddClaudeModel);
            populateCloudflareAiModelCombo(m_quickAddClaudeModel);
            selectModelComboValue(
                m_quickAddClaudeModel,
                picked.isEmpty() ? QSettings()
                                       .value(kCloudflareAiModelSetting)
                                       .toString()
                                       .trimmed()
                                 : picked);
        }
        refreshQuickAddAgentModelSelector();
        refreshComposerModelVisibilityList();
    });
}

void forkmesh::ui::tintComposerActionButton(QPushButton *button)
{
    if (!button)
        return;
    const QString name = button->property("composerGlyph").toString();
    if (name.isEmpty())
        return;
    const bool target = button->property("enterTarget").toBool();
    const bool next = Theme::isNext();
    const QColor color =
        next ? Theme::color(target ? Theme::Role::OnAccent : Theme::Role::FgSecondary)
             : Theme::remapColor(QColor(QString::fromLatin1(Theme::iconColorForButton(
                   button->objectName(), currentThemeIsDark()))));
    const int px = next ? 13 : 14;
    button->setIcon(QIcon(tintedOcticonPixmap(name, color, px)));
    button->setIconSize(QSize(px, px));
}

QString MainWindow::promptChatRoom() const
{
    if (!m_sectionStack || m_sectionStack->currentIndex() != 2 || !m_messageInput)
        return QString();
    return m_currentConversation;
}

bool MainWindow::sendPromptToChatRoom(const QString &text)
{
    const QString room = promptChatRoom();
    if (room.isEmpty())
        return false;
    if (!m_quickAddImages.isEmpty()) {
        flashMessage(QStringLiteral("Attach files to a chat room from the room's own "
                                    "message box."),
                     true);
        return true;
    }
    if (m_messageInput->isReadOnly() || !m_messageInput->isEnabled()) {
        flashMessage(QStringLiteral("You cannot post in %1.")
                         .arg(chatConversationLabel(room)),
                     true);
        return true;
    }
    if (text.isEmpty())
        return true;
    const QString draft = m_messageInput->text();
    m_messageInput->setText(text);
    sendCurrentMessage();
    const bool sent = m_messageInput->text().isEmpty();
    m_messageInput->setText(draft);
    if (sent) {
        recordQuickAddHistory(text);
        m_issueQuickAdd->clear();
    }
    schedulePromptChatPreview();
    return true;
}

int MainWindow::promptChatTargetSessionId() const
{
    const int onScreen = quickAddFollowUpAgentSessionId();
    const int target = onScreen >= 0 ? onScreen : m_selectedAgentSessionId;
    if (target < 0)
        return -1;
    for (const AgentSession &session : m_agentSessions)
        if (session.id == target)
            return target;
    return -1;
}

void MainWindow::schedulePromptChatPreview()
{
    if (!m_promptChatPreview)
        return;
    if (!m_promptChatPreviewTimer) {
        m_promptChatPreviewTimer = new QTimer(this);
        m_promptChatPreviewTimer->setSingleShot(true);
        m_promptChatPreviewTimer->setInterval(250);
        connect(m_promptChatPreviewTimer, &QTimer::timeout, this,
                &MainWindow::refreshPromptChatPreview);
    }
    if (!m_promptChatPreviewTimer->isActive())
        m_promptChatPreviewTimer->start();
}

void MainWindow::refreshPromptChatPreview()
{
    auto *preview = static_cast<ChatPreviewLabel *>(m_promptChatPreview);
    if (!preview)
        return;
    const int avatarPx = ChatPreviewLabel::kAvatarPx;
    QPixmap ownFace = roundedAvatar(effectiveUserAvatar(), avatarPx, 0.5);
    if (ownFace.isNull())
        ownFace = chatActorAvatar(QString(), chatDisplayName(), avatarPx);
    const QString room = promptChatRoom();
    const int sessionId = room.isEmpty() ? promptChatTargetSessionId() : -1;
    const AgentSession *session = findAgentSession(sessionId);
    QList<ChatPreviewLabel::Line> lines;
    if (!room.isEmpty()) {
        const QList<ChatMessage> history = m_history.value(room);
        for (int index = history.size() - 1; index >= 0 && lines.size() < 3;
             --index) {
            const ChatMessage &message = history.at(index);
            if (message.deleted || !message.threadRootId.isEmpty())
                continue;
            QString text = message.text.simplified();
            if (text.isEmpty() && message.hasFile())
                text = message.fileName;
            if (text.isEmpty())
                continue;
            const QString who = message.self ? QStringLiteral("You")
                                : message.senderName.trimmed().isEmpty()
                                    ? message.senderId
                                    : message.senderName.trimmed();
            lines.prepend({who, text, message.self,
                           message.self ? ownFace
                                        : chatActorAvatar(message.senderId,
                                                          message.senderName,
                                                          avatarPx)});
        }
    } else if (session) {
        QString agent = agentModelShortLabel(session->model);
        if (agent.isEmpty())
            agent = QStringLiteral("Agent");
        const QPixmap agentFace =
            agentModelIcon(session->provider, session->model)
                .pixmap(QSize(avatarPx, avatarPx), iconDevicePixelRatio());
        const auto lastParagraph = [](const QString &text) {
            const QStringList parts =
                text.trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (auto it = parts.crbegin(); it != parts.crend(); ++it) {
                const QString line = it->simplified();
                if (!line.isEmpty() && line != QLatin1String("```"))
                    return line;
            }
            return QString();
        };
        const QList<QJsonObject> events = m_streamEvents.value(sessionId);
        for (int index = events.size() - 1; index >= 0 && lines.size() < 3; --index) {
            const QJsonObject &event = events.at(index);
            const QString type = event.value(QStringLiteral("type")).toString();
            QString text;
            bool mine = false;
            if (type == QLatin1String("_local_user")) {
                text = event.value(QStringLiteral("text")).toString();
                mine = true;
            } else if (type == QLatin1String("_codex_agent_complete")) {
                text = event.value(QStringLiteral("text")).toString();
            } else if (type == QLatin1String("assistant") || type == QLatin1String("user")) {
                const QJsonValue content = event.value(QStringLiteral("message"))
                                               .toObject()
                                               .value(QStringLiteral("content"));
                if (content.isString()) {
                    text = content.toString();
                } else {
                    for (const QJsonValue &block : content.toArray()) {
                        const QJsonObject part = block.toObject();
                        if (part.value(QStringLiteral("type")).toString() ==
                            QLatin1String("text"))
                            text += part.value(QStringLiteral("text")).toString() +
                                    QLatin1Char('\n');
                    }
                }
                mine = type == QLatin1String("user");
            }
            text = lastParagraph(text);
            if (text.isEmpty())
                continue;
            lines.prepend({mine ? QStringLiteral("You") : agent, text, mine,
                           mine ? ownFace : agentFace});
        }
        if (lines.isEmpty()) {
            const QString reply = lastParagraph(m_lastAssistantText.value(sessionId));
            const QString asked = lastParagraph(session->prompt);
            if (!asked.isEmpty())
                lines.append({QStringLiteral("You"), asked, true, ownFace});
            if (!reply.isEmpty())
                lines.append({agent, reply, false, agentFace});
        }
    }
    m_promptChatSessionId = lines.isEmpty() || !room.isEmpty() ? -1 : sessionId;
    const bool wasShown = !preview->isHidden();
    preview->setLines(lines);
    preview->setVisible(!lines.isEmpty());
    if (m_quickAddSendToAgentButton) {
        const bool chat = !lines.isEmpty() || !room.isEmpty();
        const QString label = chat ? QStringLiteral("chat") : QStringLiteral("add");
        const QString glyph =
            chat ? QStringLiteral("comment") : QStringLiteral("paper-airplane");
        if (m_quickAddSendToAgentButton->text() != label)
            m_quickAddSendToAgentButton->setText(label);
        m_quickAddSendToAgentButton->setAccessibleName(
            !room.isEmpty() ? QStringLiteral("Send to %1").arg(chatConversationLabel(room))
            : chat          ? QStringLiteral("Send to chat")
                            : QStringLiteral("Add to agent"));
        if (m_quickAddSendToAgentButton->property("composerGlyph").toString() != glyph) {
            m_quickAddSendToAgentButton->setProperty("composerGlyph", glyph);
            tintComposerActionButton(m_quickAddSendToAgentButton);
        }
    }
    if (!m_promptWrapper)
        return;
    const int base = m_promptWrapper->property("baseAnchoredHeight").toInt();
    const QWidget *spacer =
        m_promptWrapper->findChild<QWidget *>(QStringLiteral("promptTopSpacer"));
    const int spacerHeight = spacer ? spacer->minimumHeight() : 0;
    const int anchored =
        base + (preview->isHidden() ? 0 : preview->sizeHint().height() - spacerHeight);
    if (wasShown == !preview->isHidden() &&
        (base <= 0 || anchored == m_promptAnchoredHeight))
        return;
    if (base > 0)
        m_promptAnchoredHeight = anchored;
    positionGlobalFooterOverlays();
}

void MainWindow::refreshComposerIcons()
{
    for (QPushButton *button :
         {m_quickAddSendButton, m_quickAddSendToAgentButton, m_quickAddGenieButton})
        tintComposerActionButton(button);
    if (m_quickAddModeSelector) {
        for (int row = 0; row < m_quickAddModeSelector->count() && row < 4; ++row)
            m_quickAddModeSelector->setItemIcon(row, composerModeIcon(7 + row));
    }
    refreshQuickAddSpeedSelector();
    refreshQuickAddAgentModelSelector();
}

void MainWindow::refreshQuickAddSpeedSelector()
{
    if (!m_quickAddSpeedSelector)
        return;
    const QStringList levels = agentEffortLevels();
    if (levels.isEmpty())
        return;
    QString current = QSettings()
                          .value(kClaudeEffortSetting, QStringLiteral("high"))
                          .toString()
                          .trimmed()
                          .toLower();
    if (!levels.contains(current)) {
        current = levels.contains(QStringLiteral("high")) ? QStringLiteral("high")
                                                          : levels.last();
        QSettings().setValue(kClaudeEffortSetting, current);
    }
    const QSignalBlocker block(m_quickAddSpeedSelector);
    m_quickAddSpeedSelector->clear();
    for (int i = 0; i < levels.size(); ++i)
        m_quickAddSpeedSelector->addItem(
            Theme::isNext() ? composerEffortIcon(i, levels.size())
                            : agentControlIcon(11 + qMin(i, 4)),
            agentEffortLabel(levels.at(i)), levels.at(i));
    const int idx = m_quickAddSpeedSelector->findData(current);
    m_quickAddSpeedSelector->setCurrentIndex(idx >= 0 ? idx : 0);
    const QString description =
        QStringLiteral("Reasoning effort: %1")
            .arg(m_quickAddSpeedSelector->currentText());
    m_quickAddSpeedSelector->setAccessibleName(description);
    m_quickAddSpeedSelector->setToolTip(
        description +
        QStringLiteral(". Higher levels are slower and more thorough."));
}

QList<ComposerModelChoice> MainWindow::composerModelCatalog() const
{
    // Building the catalog constructs three throw-away QComboBox widgets
    // (each a full widget polish against the app stylesheet) and parses the
    // cached Cloudflare model list; showAgentSession() asks for it up to
    // four times per call. Memoize on the inputs: the live Claude list and
    // the three settings the populate helpers read.
    QString inputs = QString::fromUtf8(
        QJsonDocument(m_liveClaudeModels).toJson(QJsonDocument::Compact));
    {
        const QSettings settings;
        inputs += QLatin1Char('|') +
                  QString::fromLatin1(
                      settings.value(kCloudflareAiModelsCacheSetting).toByteArray()) +
                  QLatin1Char('|') +
                  QString::fromLatin1(
                      settings.value(kCodexModelsCacheSetting).toByteArray()) +
                  QLatin1Char('|') +
                  settings.value(kLocalLlamaModelSetting).toString().trimmed() +
                  QLatin1Char('|') +
                  QString::fromLatin1(
                      settings.value(kMarketplaceLocalModelsCacheSetting)
                          .toByteArray());
    }
    if (inputs == m_composerModelCatalogInputs && !m_composerModelCatalog.isEmpty())
        return m_composerModelCatalog;

    QList<ComposerModelChoice> choices;

    QComboBox claudeModels;
    populateClaudeModelCombo(&claudeModels);
    if (!m_liveClaudeModels.isEmpty())
        mergeLiveClaudeModels(&claudeModels, m_liveClaudeModels);
    for (int i = 0; i < claudeModels.count(); ++i) {
        const QString id = claudeModels.itemData(i).toString();
        const int icon =
            id.compare(kClaudeAutoModelId, Qt::CaseInsensitive) == 0
                ? 7
                : agentModelFaceIconIndex(QStringLiteral("claude-code"), id);
        choices.append(ComposerModelChoice{
            QStringLiteral("claude-code"), id,
            compactModelName(claudeModels.itemText(i)),
            QStringLiteral("Claude Code"), QString(), agentControlIcon(icon),
            true});
    }

    QComboBox codexModels;
    populateCodexModelCombo(&codexModels);
    for (int i = 0; i < codexModels.count(); ++i) {
        const QString id = codexModels.itemData(i).toString();
        choices.append(ComposerModelChoice{
            kCodexProvider, id, codexModels.itemText(i),
            QStringLiteral("Codex"), QString(),
            agentModelIcon(kCodexProvider, id), true});
    }

    choices.append(ComposerModelChoice{
        QStringLiteral("openai"), QString(), QStringLiteral("OpenAI API"),
        QStringLiteral("OpenAI API"), QStringLiteral("Headless OpenAI API agent"),
        agentModelIcon(QStringLiteral("openai"), QString()), false});
    choices.append(ComposerModelChoice{
        QStringLiteral("claude-api"), QString(), QStringLiteral("Claude API"),
        QStringLiteral("Claude API"), QStringLiteral("Headless Claude API agent"),
        agentModelIcon(QStringLiteral("claude-api"), QString()), false});

    QComboBox cloudflareModels;
    populateCloudflareAiModelCombo(&cloudflareModels);
    for (int i = 0; i < cloudflareModels.count(); ++i) {
        const QString label = cloudflareModels.itemText(i);
        choices.append(ComposerModelChoice{
            kCloudflareAiProvider, cloudflareModels.itemData(i).toString(), label,
            QStringLiteral("Cloudflare AI"),
            QStringLiteral("%1 · Cloudflare AI — headless agent on the relay's "
                           "Workers AI").arg(label),
            agentModelIcon(kCloudflareAiProvider, QString()), false});
    }
    const QString localModel =
        QSettings().value(kLocalLlamaModelSetting).toString().trimmed();
    choices.append(ComposerModelChoice{
        kLocalLlamaProvider, localModel,
        localModel.isEmpty() ? QStringLiteral("Local Llama (auto)") : localModel,
        QStringLiteral("Local Llama"),
        QStringLiteral("Runs entirely on the installed local model server"),
        agentModelIcon(kLocalLlamaProvider, localModel), false});
    const QJsonArray packageModels = QJsonDocument::fromJson(
        QSettings().value(kMarketplaceLocalModelsCacheSetting).toByteArray())
                                         .array();
    for (const QJsonValue &value : packageModels) {
        const QJsonObject model = value.toObject();
        const QString provider =
            model.value(QStringLiteral("provider")).toString();
        const QString id = model.value(QStringLiteral("id")).toString();
        const QString label = model.value(QStringLiteral("label")).toString();
        const QString agentName =
            model.value(QStringLiteral("agentName")).toString();
        if (provider.isEmpty() || id.isEmpty())
            continue;
        choices.append(ComposerModelChoice{
            provider, id, label.isEmpty() ? id : label,
            agentName.isEmpty() ? QStringLiteral("Marketplace model") : agentName,
            QStringLiteral("%1 · local model installed from ForkMesh Market")
                .arg(label.isEmpty() ? id : label),
            agentModelIcon(provider, id), false});
    }
    m_composerModelCatalogInputs = inputs;
    m_composerModelCatalog = choices;
    return choices;
}

// The agent+model pairs this node runs most, most-used first. The composer
// offers the whole catalog ranked by merged success; a one-click action wants
// the short list of what actually gets used here, so the ranking is run count
// (a model you keep reaching for is the one you would pick again) with the
// catalog's power ranking breaking ties — which is also what orders the list on
// a fresh node that has no runs to count yet. Models hidden from the composer
// stay hidden, and only the ranked CLI rows are eligible: an agent that fixes a
// pull request has to work in the branch's checkout.
QList<ComposerModelChoice> MainWindow::mostUsedAgentModels(int limit) const
{
    if (limit <= 0)
        return {};
    QHash<QString, int> runs;
    if (m_agentStore) {
        const QHash<QString, AgentModelOutcome> retired =
            m_agentStore->retiredModelOutcomes();
        for (auto it = retired.constBegin(); it != retired.constEnd(); ++it)
            runs[it.key()] += it.value().runs;
    }
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        const QString key =
            AgentStore::modelOutcomeKey(session.provider, session.model);
        if (!key.isEmpty())
            ++runs[key];
    }
    const QSet<QString> hidden = hiddenComposerModels();
    struct Ranked {
        ComposerModelChoice choice;
        int runs = 0;
        int power = 0;
    };
    QList<Ranked> ranked;
    for (const ComposerModelChoice &entry : composerModelCatalog()) {
        if (!entry.ranked ||
            hidden.contains(composerModelKey(entry.provider, entry.model)))
            continue;
        ranked.append(
            Ranked{entry,
                   runs.value(AgentStore::modelOutcomeKey(entry.provider,
                                                          entry.model)),
                   agentModelPowerRank(entry.model, entry.label)});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Ranked &a, const Ranked &b) {
                         if (a.runs != b.runs)
                             return a.runs > b.runs;
                         return a.power > b.power;
                     });
    QList<ComposerModelChoice> out;
    for (const Ranked &row : ranked) {
        if (out.size() >= limit)
            break;
        out.append(row.choice);
    }
    return out;
}

void MainWindow::refreshQuickAddAgentModelSelector()
{
    if (!m_quickAddAgentModelSelector || !m_quickAddAgentProvider ||
        !m_quickAddClaudeModel)
        return;
    const QString selectedProvider =
        m_quickAddAgentProvider->currentData().toString();
    const QString selectedModel =
        agentSelectorTakesModel(selectedProvider)
            ? selectedModelComboValue(m_quickAddClaudeModel)
            : QString();
    fillAgentModelSelector(
        m_quickAddAgentModelSelector, selectedProvider, selectedModel, true,
        QStringLiteral("Choose the agent and model that will handle this "
                       "prompt."));
    if (!Theme::isNext())
        return;
    const QSignalBlocker block(m_quickAddAgentModelSelector);
    for (int row = 0; row < m_quickAddAgentModelSelector->count(); ++row)
        m_quickAddAgentModelSelector->setItemIcon(
            row, composerMonoIcon(composerProviderOcticon(
                     m_quickAddAgentModelSelector->itemData(row).toString())));
}

// Every combined agent+model picker on screen. Call this, not one of the two
// below, whenever the catalog or its merged-success ranking moves: the composer
// and the issue sidebar offer the same list and must not drift apart.
void MainWindow::refreshAgentModelSelectors()
{
    refreshQuickAddAgentModelSelector();
    refreshIssueAgentModelSelector();
    refreshPullFixModelCombo();
}

void MainWindow::refreshIssueAgentModelSelector()
{
    if (!m_issueAgentSelector || !m_issueAgentProvider || !m_issueAgentModel)
        return;
    const QString selectedProvider =
        m_issueAgentProvider->currentData().toString();
    const QString selectedModel =
        agentSelectorTakesModel(selectedProvider)
            ? selectedModelComboValue(m_issueAgentModel)
            : QString();
    // No prompt-action rows: the issue already exists, so the only question
    // here is which agent runs on it.
    fillAgentModelSelector(
        m_issueAgentSelector, selectedProvider, selectedModel, false,
        QStringLiteral("Choose the agent and model that will run on this "
                       "issue."));
}

// One icon-rich list of every (agent, model) pair, most-merged first, shared by
// the composer and the issue sidebar so both offer the same line-up. The two
// hidden provider/model combos stay the source of truth; this only paints them
// as one row and reports the pair back through the caller's slot.
void MainWindow::fillAgentModelSelector(QComboBox *selector,
                                        const QString &selectedProvider,
                                        const QString &selectedModel,
                                        bool includeManual,
                                        const QString &fallbackTip)
{
    if (!selector)
        return;
    const QSignalBlocker blocker(selector);
    selector->clear();

    struct Choice {
        QIcon icon;
        QString label;
        QString provider;
        QString model;
        QString agentName; // which CLI/API runs it, for the tooltip
        int mergedCount = 0; // higher sorts nearer the top
        int runCount = 0;
        int powerRank = 0; // stable tie-breaker for equally-used models
    };
    const QSet<QString> hidden = hiddenComposerModels();
    const auto rowIsVisible = [&](const QString &provider,
                                  const QString &model) {
        if (!hidden.contains(composerModelKey(provider, model)))
            return true;
        return provider == selectedProvider &&
               (selectedModel.isEmpty() || model == selectedModel);
    };
    QHash<QString, int> modelMergedCounts;
    QHash<QString, int> modelRunCounts;
    if (m_agentStore) {
        const QHash<QString, AgentModelOutcome> retired =
            m_agentStore->retiredModelOutcomes();
        for (auto it = retired.constBegin(); it != retired.constEnd(); ++it) {
            modelRunCounts[it.key()] += it.value().runs;
            modelMergedCounts[it.key()] += it.value().merged;
        }
    }
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        const QString key =
            AgentStore::modelOutcomeKey(session.provider, session.model);
        if (key.isEmpty())
            continue;
        ++modelRunCounts[key];
        if (session.merged)
            ++modelMergedCounts[key];
    }
    const QList<ComposerModelChoice> catalog = composerModelCatalog();
    QList<Choice> models;
    QList<ComposerModelChoice> unranked;
    for (const ComposerModelChoice &entry : catalog) {
        if (!rowIsVisible(entry.provider, entry.model))
            continue;
        if (!entry.ranked) {
            unranked.append(entry);
            continue;
        }
        const QString key =
            AgentStore::modelOutcomeKey(entry.provider, entry.model);
        models.append(Choice{entry.icon, entry.label,
                             entry.provider, entry.model, entry.agentName,
                             modelMergedCounts.value(key),
                             modelRunCounts.value(key),
                             agentModelPowerRank(entry.model, entry.label)});
    }

    std::stable_sort(models.begin(), models.end(),
                     [](const Choice &a, const Choice &b) {
                         if (a.mergedCount != b.mergedCount)
                             return a.mergedCount > b.mergedCount;
                         return a.powerRank > b.powerRank;
                     });

    auto addChoice = [selector](const QIcon &icon, const QString &label,
                                const QString &provider, const QString &model,
                                const QString &tooltip,
                                const QString &description = QString()) {
        const int row = selector->count();
        selector->addItem(icon, label, provider);
        selector->setItemData(row, model, Qt::UserRole + 1);
        if (!tooltip.isEmpty())
            selector->setItemData(row, tooltip, Qt::ToolTipRole);
        if (!description.isEmpty())
            selector->setItemData(row, description,
                                  kAgentChoiceDescriptionRole);
    };
    if (includeManual) {
        addChoice(agentControlIcon(10), QStringLiteral("Manual · create issue"),
                  QStringLiteral("manual"), QString(),
                  QStringLiteral("File an issue from this prompt instead of "
                                 "starting an agent"));
        addChoice(themedOcticon(QStringLiteral("mastodon"), QColor("#6364ff"), 22),
                  QStringLiteral("ActivityPub · post"),
                  kActivityPubPromptProvider, QString(),
                  QStringLiteral("Publish this prompt to the repository's "
                                 "ActivityPub followers"));
        addChoice(themedOcticon(QStringLiteral("pencil"), QColor("#8b949e"), 22),
                  QStringLiteral("Take a note"), kNotePromptProvider, QString(),
                  QStringLiteral("Save this prompt as a note on this computer "
                                 "instead of starting an agent"));
        addChoice(themedOcticon(QStringLiteral("people"), QColor("#8b949e"), 22),
                  QStringLiteral("Task · assign"), kTaskPromptProvider,
                  QString(),
                  QStringLiteral("Create an organization task and assign it to "
                                 "the member picked beside this menu"));
    }
    const QString chosenIdentity = topBarUserName().trimmed();
    QHash<QString, QString> providerIdentities;
    for (const Choice &choice : models) {
        const QString label = choice.label;
        const QString mergedNote =
            QStringLiteral("%1 merged").arg(choice.mergedCount);
        QString toolTip = QStringLiteral("%1 · %2").arg(choice.label,
                                                        choice.agentName);
        toolTip += QStringLiteral("\nMerged success: %1 of %2 runs")
                       .arg(choice.mergedCount)
                       .arg(choice.runCount);
        if (!providerIdentities.contains(choice.provider)) {
            const AgentAccountProfile account =
                activeAgentAccount(choice.provider);
            providerIdentities.insert(
                choice.provider,
                agentCliIdentityLabel(choice.provider, account.configDir));
        }
        const QString identity = providerIdentities.value(choice.provider);
        if (!identity.isEmpty())
            toolTip = QStringLiteral("Account: %1\n%2").arg(identity, toolTip);
        else if (!chosenIdentity.isEmpty())
            toolTip = QStringLiteral("Account: %1\n%2").arg(chosenIdentity, toolTip);
        addChoice(choice.icon, label, choice.provider, choice.model, toolTip,
                  mergedNote);
    }

    for (const ComposerModelChoice &entry : unranked)
        addChoice(entry.icon, entry.label, entry.provider,
                  entry.model, entry.tooltip);
    if (includeManual)
        addChoice(themedOcticon(QStringLiteral("gear"), QColor("#58a6ff"), 22),
                  QStringLiteral("Edit models…"), kEditComposerModelsRow,
                  QString(),
                  QStringLiteral("Open the Models page and tick the models this "
                                 "menu lists"));
    selector->setMaxVisibleItems(qMax(selector->maxVisibleItems(),
                                      selector->count()));

    int selected = -1;
    for (int i = 0; i < selector->count(); ++i) {
        if (selector->itemData(i).toString() != selectedProvider)
            continue;
        const QString rowModel =
            selector->itemData(i, Qt::UserRole + 1).toString();
        if (selectedModel.isEmpty() || rowModel == selectedModel) {
            selected = i;
            break;
        }
    }
    selector->setCurrentIndex(selected >= 0 ? selected : 0);
    const int current = selector->currentIndex();
    const QString currentTip =
        current >= 0 ? selector->itemData(current, Qt::ToolTipRole).toString()
                     : QString();
    selector->setAccessibleName(
        QStringLiteral("Agent and model: %1")
            .arg(currentTip.isEmpty() ? selector->currentText() : currentTip));
    selector->setToolTip(
        currentTip.isEmpty()
            ? fallbackTip
            : QStringLiteral("%1. Click to choose a different agent or model.")
                  .arg(currentTip));
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testRefreshQuickAddAgentModelSelector()
{
    refreshQuickAddAgentModelSelector();
}

QString MainWindow::testQuickAddAgentModelLabel(const QString &model) const
{
    if (!m_quickAddAgentModelSelector)
        return QString();
    for (int row = 0; row < m_quickAddAgentModelSelector->count(); ++row) {
        if (m_quickAddAgentModelSelector->itemData(row, Qt::UserRole + 1)
                .toString() == model)
            return m_quickAddAgentModelSelector->itemText(row);
    }
    return QString();
}

QString MainWindow::testQuickAddAgentModelMergedNote(const QString &model) const
{
    if (!m_quickAddAgentModelSelector)
        return QString();
    for (int row = 0; row < m_quickAddAgentModelSelector->count(); ++row) {
        if (m_quickAddAgentModelSelector->itemData(row, Qt::UserRole + 1)
                .toString() == model)
            return m_quickAddAgentModelSelector
                ->itemData(row, kAgentChoiceDescriptionRole)
                .toString();
    }
    return QString();
}

QString MainWindow::testPromptOverlayPlacement() const
{
    if (m_promptOverlayDetached)
        return QStringLiteral("detached");
    return m_promptOverlayFloating ? QStringLiteral("floating")
                                   : QStringLiteral("anchored");
}
#endif // FORKMESH_WINDOW_TESTS

void MainWindow::refreshClaudeEffortLevels()
{
    if (m_claudeEffortProbe)
        return;
    auto *proc = new QProcess(this);
    m_claudeEffortProbe = proc;
    m_claudeEffortProbeBuf.clear();
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        m_claudeEffortProbeBuf += proc->readAllStandardOutput();
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc](int, QProcess::ExitStatus) {
                if (m_claudeEffortProbe == proc)
                    m_claudeEffortProbe = nullptr;
                const QString help = QString::fromUtf8(m_claudeEffortProbeBuf);
                m_claudeEffortProbeBuf.clear();
                proc->deleteLater();
                static const QRegularExpression effortLine(
                    QStringLiteral("--effort[^\\n]*"));
                const QRegularExpressionMatch line = effortLine.match(help);
                if (!line.hasMatch())
                    return;
                static const QRegularExpression choice(
                    QStringLiteral("\"([A-Za-z][A-Za-z0-9_-]*)\""));
                QStringList levels;
                QRegularExpressionMatchIterator it =
                    choice.globalMatch(line.captured(0));
                while (it.hasNext()) {
                    const QString level = it.next().captured(1).toLower();
                    if (!levels.contains(level))
                        levels << level;
                }
                if (levels.isEmpty()) {
                    static const QRegularExpression bare(
                        QStringLiteral("choices:\\s*([^)]+)"));
                    const QRegularExpressionMatch list = bare.match(line.captured(0));
                    if (!list.hasMatch())
                        return;
                    static const QRegularExpression separator(
                        QStringLiteral("[,\\s]+"));
                    static const QRegularExpression wordOnly(
                        QStringLiteral("^[a-z][a-z0-9_-]*$"));
                    for (const QString &part :
                         list.captured(1).split(separator, Qt::SkipEmptyParts)) {
                        const QString level = part.trimmed().toLower();
                        if (wordOnly.match(level).hasMatch() && !levels.contains(level))
                            levels << level;
                    }
                }
                if (levels.isEmpty())
                    return;
                QSettings().setValue(kClaudeEffortLevelsCacheSetting, levels);
                refreshQuickAddSpeedSelector();
                if (m_slashActionsPopup && m_slashActionsPopup->isVisible())
                    populateSlashActionsList();
            });
    const QString probeDir = forkmesh::vm::active()
                                 ? forkmesh::vm::workspaceRoot()
                                 : QDir::currentPath();
    const forkmesh::vm::LaunchCommand probe = forkmesh::vm::isolateCommand(
        QStringLiteral("bash"),
        {QStringLiteral("-lc"), QStringLiteral("claude --help 2>/dev/null")},
        probeDir, false);
    if (!probe.error.isEmpty()) {
        m_claudeEffortProbe = nullptr;
        proc->deleteLater();
        return;
    }
    proc->setWorkingDirectory(probeDir);
    proc->start(probe.program, probe.arguments);
}

// Probes the live `claude` CLI for its slash-command list via the same
// control-protocol `initialize` request the VS Code extension sends
// pipe one control_request in, read the control_response, then
// tear the process down — this never runs a real turn. Cached for the rest of
// the app run; a no-op once loaded or while a probe is already in flight.
void MainWindow::refreshClaudeSlashCommands()
{
    if (m_claudeSlashCommandsLoaded || m_claudeSlashProbe)
        return;
    auto *proc = new QProcess(this);
    m_claudeSlashProbe = proc;
    m_claudeSlashProbeBuf.clear();
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        m_claudeSlashProbeBuf += proc->readAllStandardOutput();
        int nl;
        while ((nl = m_claudeSlashProbeBuf.indexOf('\n')) >= 0) {
            const QByteArray line = m_claudeSlashProbeBuf.left(nl);
            m_claudeSlashProbeBuf.remove(0, nl + 1);
            const QJsonObject obj = QJsonDocument::fromJson(line).object();
            if (obj.value(QStringLiteral("type")).toString() !=
                QLatin1String("control_response"))
                continue;
            const QJsonArray commands = obj.value(QStringLiteral("response"))
                                             .toObject()
                                             .value(QStringLiteral("response"))
                                             .toObject()
                                             .value(QStringLiteral("commands"))
                                             .toArray();
            m_claudeSlashCommands.clear();
            for (const QJsonValue &v : commands) {
                const QJsonObject c = v.toObject();
                const QString name = c.value(QStringLiteral("name")).toString();
                if (name.isEmpty())
                    continue;
                m_claudeSlashCommands.append(
                    {name, c.value(QStringLiteral("description")).toString(),
                     c.value(QStringLiteral("argumentHint")).toString()});
            }
            m_claudeSlashCommandsLoaded = true;
            if (m_slashActionsPopup && m_slashActionsPopup->isVisible())
                populateSlashActionsList();
            if (proc->state() != QProcess::NotRunning)
                proc->kill(); // the initialize handshake is all we needed
        }
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc](int, QProcess::ExitStatus) {
                if (m_claudeSlashProbe == proc)
                    m_claudeSlashProbe = nullptr;
                proc->deleteLater();
            });
    connect(proc, &QProcess::started, this, [proc] {
        const QJsonObject req{
            {QStringLiteral("type"), QStringLiteral("control_request")},
            {QStringLiteral("request_id"), QStringLiteral("forkmesh-slash-probe")},
            {QStringLiteral("request"),
             QJsonObject{{QStringLiteral("subtype"), QStringLiteral("initialize")}}}};
        proc->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    });
    const QString probeDir = forkmesh::vm::active()
                                 ? forkmesh::vm::workspaceRoot()
                                 : QDir::currentPath();
    const forkmesh::vm::LaunchCommand probe = forkmesh::vm::isolateCommand(
        QStringLiteral("bash"),
        {QStringLiteral("-lc"),
         QStringLiteral("exec claude --print --input-format stream-json "
                        "--output-format stream-json --verbose")},
        probeDir, false);
    if (!probe.error.isEmpty()) {
        m_claudeSlashProbe = nullptr;
        proc->deleteLater();
        return;
    }
    proc->setWorkingDirectory(probeDir);
    proc->start(probe.program, probe.arguments);
}

void MainWindow::mentionProjectFileInQuickAdd()
{
    if (!m_issueQuickAdd)
        return;
    QString baseDir;
    const int idx = issuesRepoIndex();
    if (idx >= 0 && idx < m_repositories.size())
        baseDir = m_repositories.at(idx).localPath;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Mention a file from this project"), baseDir);
    if (path.isEmpty()) {
        m_issueQuickAdd->setFocus();
        return;
    }
    QString mention = path;
    if (!baseDir.isEmpty()) {
        const QDir dir(baseDir);
        const QString rel = dir.relativeFilePath(path);
        if (!rel.startsWith(QStringLiteral("..")))
            mention = rel;
    }
    QTextCursor cursor = m_issueQuickAdd->textCursor();
    cursor.insertText(QStringLiteral("@%1 ").arg(mention));
    m_issueQuickAdd->setTextCursor(cursor);
    m_issueQuickAdd->setFocus();
}


void MainWindow::initializeWorldSpeechBridge()
{
    if (m_worldSpeechBridge)
        return;
    m_worldSpeechBridge = new WorldSpeechBridge(this);
    m_worldSpeechDraftEdit = new QPlainTextEdit(this);
    m_worldSpeechDraftEdit->setVisible(false);
    m_worldSpeechDraftEdit->setObjectName(
        QStringLiteral("worldSpeechTranscriptDraft"));
    m_worldSpeechHiddenMicButton = new QPushButton(this);
    m_worldSpeechHiddenMicButton->setVisible(false);

    connect(m_worldSpeechBridge, &WorldSpeechBridge::auditEvent, this,
            [this](const QString &event) {
                logSystem(QStringLiteral("World voice: ") + event);
            });
    connect(m_worldSpeechBridge, &WorldSpeechBridge::captureRequested, this,
            [this](const QString &captureId, const QString &destination) {
                if (!m_worldSpeechBridge || captureId.isEmpty())
                    return;
                if ((m_voiceRecordProc &&
                     m_voiceRecordProc->state() != QProcess::NotRunning) ||
                    (m_voiceTranscribeProc &&
                     m_voiceTranscribeProc->state() != QProcess::NotRunning) ||
                    m_voiceRecording) {
                    m_worldSpeechBridge->publishError(
                        captureId,
                        QStringLiteral("Desktop microphone is already in use."));
                    return;
                }
                if (!voiceInputReady()) {
                    m_worldSpeechBridge->publishError(
                        captureId,
                        QStringLiteral("Set up a local speech-to-text engine in "
                                       "Qt Settings > Voice."));
                    return;
                }
                const AudioRecorderCommand recorder =
                    audioRecorderFor(QStringLiteral("probe.wav"));
                if (recorder.program.isEmpty()) {
                    m_worldSpeechBridge->publishError(
                        captureId,
                        QStringLiteral("No supported local microphone recorder "
                                       "is installed."));
                    return;
                }
                m_worldSpeechCancelPending = false;
                m_worldSpeechCaptureId = captureId;
                m_worldSpeechDraftEdit->clear();
                m_worldSpeechDraftEdit->setPlaceholderText(
                    QStringLiteral("World %1 transcript").arg(destination));
                startVoiceCaptureFor(m_worldSpeechDraftEdit,
                                     m_worldSpeechHiddenMicButton);
                if (!m_voiceRecording) {
                    m_worldSpeechBridge->publishError(
                        captureId,
                        QStringLiteral("The local microphone could not start."));
                    m_worldSpeechCaptureId.clear();
                } else if (m_worldSpeechStatusLabel) {
                    m_worldSpeechStatusLabel->setText(
                        QStringLiteral("World is recording locally for the %1 "
                                       "composer. Use Stop or Cancel in the "
                                       "browser.")
                            .arg(destination));
                }
            });
    connect(m_worldSpeechBridge, &WorldSpeechBridge::stopRequested, this,
            [this](const QString &captureId) {
                if (captureId != m_worldSpeechCaptureId)
                    return;
                stopVoiceCapture();
                if (m_worldSpeechStatusLabel)
                    m_worldSpeechStatusLabel->setText(
                        QStringLiteral("World capture stopped; transcribing "
                                       "locally."));
            });
    connect(m_worldSpeechBridge, &WorldSpeechBridge::cancelRequested, this,
            [this](const QString &captureId) {
                if (captureId == m_worldSpeechCaptureId)
                    cancelWorldVoiceCapture();
            });
}

void MainWindow::createWorldSpeechPairing()
{
    if (!m_worldSpeechBridge)
        initializeWorldSpeechBridge();
    if (!m_worldSpeechBridge || !m_worldSpeechOriginEdit)
        return;
    const QString origin = m_worldSpeechOriginEdit->text().trimmed();
    const QJsonObject capability =
        m_worldSpeechBridge->issuePairing(origin, 120);
    if (capability.isEmpty()) {
        if (m_worldSpeechStatusLabel)
            m_worldSpeechStatusLabel->setText(
                QStringLiteral("Pairing refused. Enter one exact http(s) origin "
                               "without a path, wildcard, query, or fragment."));
        return;
    }
    QSettings().setValue(kWorldSpeechOriginSetting, origin);
    const QString secret =
        capability.value(QStringLiteral("secret")).toString();
    if (m_worldSpeechPairCodeEdit) {
        m_worldSpeechPairCodeEdit->setText(secret);
        m_worldSpeechPairCodeEdit->selectAll();
        m_worldSpeechPairCodeEdit->setFocus();
    }
    if (m_worldSpeechPairButton)
        m_worldSpeechPairButton->setEnabled(false);
    if (m_worldSpeechRevokeButton)
        m_worldSpeechRevokeButton->setEnabled(true);
    if (m_worldSpeechStatusLabel)
        m_worldSpeechStatusLabel->setText(
            QStringLiteral("Listening on 127.0.0.1:%1. This one-time code "
                           "expires at %2 and is valid only for %3.")
                .arg(capability.value(QStringLiteral("port")).toInt())
                .arg(capability.value(QStringLiteral("expiresAt")).toString(),
                     capability.value(QStringLiteral("origin")).toString()));

}

void MainWindow::revokeWorldSpeechPairing()
{
    if (m_worldSpeechBridge)
        m_worldSpeechBridge->revokeAll();
    if (m_worldSpeechPairCodeEdit)
        m_worldSpeechPairCodeEdit->clear();
    if (m_worldSpeechPairButton)
        m_worldSpeechPairButton->setEnabled(true);
    if (m_worldSpeechRevokeButton)
        m_worldSpeechRevokeButton->setEnabled(false);
    if (m_worldSpeechStatusLabel)
        m_worldSpeechStatusLabel->setText(
            QStringLiteral("Bridge capabilities revoked. No browser can "
                           "control the microphone."));
}

void MainWindow::cancelWorldVoiceCapture()
{
    m_worldSpeechCancelPending = true;
    if (m_voiceLiveTimer)
        m_voiceLiveTimer->stop();
    stopVoiceLevelMeter();
    if (m_voiceRecordProc &&
        m_voiceRecordProc->state() != QProcess::NotRunning)
        m_voiceRecordProc->kill();
    if (m_voiceTranscribeProc &&
        m_voiceTranscribeProc->state() != QProcess::NotRunning)
        m_voiceTranscribeProc->kill();
    m_voiceRecording = false;
    hideVoiceTranscribeSpinner();
    QFile::remove(m_voiceWavPath);
    QFile::remove(m_voiceWavPath + QStringLiteral(".out.txt"));
    m_voiceInsertPos = -1;
    m_voiceInsertLen = 0;
    m_voiceLastPreview.clear();
    if (m_worldSpeechDraftEdit)
        m_worldSpeechDraftEdit->clear();
    m_worldSpeechCaptureId.clear();
    updateVoiceInputButton();
    if (m_worldSpeechStatusLabel)
        m_worldSpeechStatusLabel->setText(
            QStringLiteral("World microphone capture cancelled locally."));
}

void MainWindow::updateVoiceInputButton()
{
    const bool enabled = voiceInputEnabled();
    const bool ready = voiceInputReady();
    if (!enabled && m_voiceRecording &&
        m_voiceActiveButton != m_worldSpeechHiddenMicButton)
        stopVoiceCapture();
    if (m_quickAddMicButton) {
        m_quickAddMicButton->setVisible(enabled);
        m_quickAddMicButton->setEnabled(enabled);
        if (!(m_voiceRecording && m_voiceActiveButton == m_quickAddMicButton)) {
            setOcticon(m_quickAddMicButton, "mic", 16);
            m_quickAddMicButton->setToolTip(
                ready ? QString::fromUtf8(
                            "Speak your prompt \xE2\x80\x94 hold to record, release "
                            "to transcribe.\nVoice model: %1")
                            .arg(voiceModelLabel())
                      : QString::fromUtf8(
                            "Speech-to-text isn't set up yet \xE2\x80\x94 click to open "
                            "Settings and set up voice input."));
        }
    }
    for (QPushButton *b : m_voiceButtons) {
        if (!b)
            continue;
        b->setVisible(enabled && ready);
        if (m_voiceRecording && m_voiceActiveButton == b)
            continue;
        setOcticon(b, "mic", 16);
    }
}

void MainWindow::stopVoiceCapture()
{
    if (!m_voiceRecording)
        return;
    if (m_voiceLiveTimer)
        m_voiceLiveTimer->stop();
    stopVoiceLevelMeter();
    if (m_voiceRecordProc && m_voiceRecordProc->state() != QProcess::NotRunning)
        m_voiceRecordProc->terminate();
}

void MainWindow::startVoiceCapture()
{
    if (!voiceInputEnabled()) {
        updateVoiceInputButton();
        return;
    }
    startVoiceCaptureFor(m_issueQuickAdd, m_quickAddMicButton);
}

QPushButton *MainWindow::makeVoiceButton(MarkdownEditor *composer)
{
    auto *btn = new QPushButton;
    btn->setObjectName("ghostButton");
    btn->setCursor(Qt::PointingHandCursor);
    setOcticon(btn, "mic", 16);
    btn->setToolTip(QString::fromUtf8("Speak your comment \xE2\x80\x94 hold to "
                                      "record, release to transcribe.\nVoice "
                                      "model: %1")
                        .arg(voiceModelLabel()));
    btn->setVisible(voiceInputEnabled() && voiceInputReady());
    connect(btn, &QPushButton::pressed, this, [this, composer, btn] {
        if (!voiceInputEnabled())
            return;
        if (!m_voiceRecording && composer)
            composer->showWriteArea();
        startVoiceCaptureFor(composer ? composer->sourceEdit() : nullptr, btn);
    });
    connect(btn, &QPushButton::released, this, &MainWindow::stopVoiceCapture);
    m_voiceButtons.append(btn);
    return btn;
}

void MainWindow::startVoiceCaptureFor(QPlainTextEdit *target, QPushButton *button)
{
    if (!button || !target)
        return;

    if (m_voiceRecording)
        return;

    if (!voiceInputReady()) {
        updateVoiceInputButton();
        openVoiceSettings();
        return;
    }
    if (m_voiceTranscribeProc &&
        m_voiceTranscribeProc->state() != QProcess::NotRunning) {
        const QString busy =
            QString::fromUtf8("still transcribing the last clip\xE2\x80\xA6");
        const QString prev = target->placeholderText();
        if (prev != busy) {
            target->setPlaceholderText(busy);
            QTimer::singleShot(1200, target, [target, busy, prev] {
                if (target->placeholderText() == busy)
                    target->setPlaceholderText(prev);
            });
        }
        return;
    }

    m_voiceTargetEdit = target;
    m_voiceActiveButton = button;
    m_voiceIdlePlaceholder = target->placeholderText();

    m_voiceWavPath =
        QDir(QDir::tempPath())
            .filePath(QStringLiteral("forkmesh-voice-%1.wav")
                          .arg(QDateTime::currentMSecsSinceEpoch()));
    const AudioRecorderCommand rec = audioRecorderFor(m_voiceWavPath);
    if (rec.program.isEmpty()) {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        logSystem(
            "Voice input needs ffmpeg to capture the microphone. Install it "
            "(macOS: \"brew install ffmpeg\"; Windows: ffmpeg.org) and make sure "
            "it's on your PATH.");
        target->setPlaceholderText("no recorder found (install ffmpeg)");
#else
        logSystem(
            "Voice input needs a microphone recorder. Install one of: arecord "
            "(alsa-utils), parecord (pulseaudio-utils) or ffmpeg.");
        target->setPlaceholderText(
            "no recorder found (install arecord / parecord / ffmpeg)");
#endif
        return;
    }

    auto *proc = new QProcess(this);
    m_voiceRecordProc = proc;
    connect(proc, &QProcess::finished, this,
            [this, proc](int, QProcess::ExitStatus) {
                m_voiceRecording = false;
                if (m_voiceLiveTimer)
                    m_voiceLiveTimer->stop();
                stopVoiceLevelMeter();
                if (m_voiceRecordProc == proc)
                    m_voiceRecordProc = nullptr;
                const QString err =
                    QString::fromUtf8(proc->readAllStandardError()).trimmed();
                proc->deleteLater();
                updateVoiceInputButton();
                if (m_worldSpeechCancelPending &&
                    m_voiceTargetEdit == m_worldSpeechDraftEdit) {
                    QFile::remove(m_voiceWavPath);
                    QFile::remove(m_voiceWavPath +
                                  QStringLiteral(".out.txt"));
                    return;
                }
                if (!QFileInfo::exists(m_voiceWavPath) ||
                    QFileInfo(m_voiceWavPath).size() < 1024) {
                    if (!err.isEmpty())
                        logSystem("Microphone capture failed: " + err.right(200));
                    if (m_voiceTargetEdit)
                        m_voiceTargetEdit->setPlaceholderText(m_voiceIdlePlaceholder);
                    m_voiceInsertPos = -1;
                    m_voiceInsertLen = 0;
                    QFile::remove(m_voiceWavPath);
                    if (m_worldSpeechBridge &&
                        m_voiceTargetEdit == m_worldSpeechDraftEdit &&
                        !m_worldSpeechCaptureId.isEmpty()) {
                        if (err.isEmpty())
                            m_worldSpeechBridge->publishFinal(
                                m_worldSpeechCaptureId, QString());
                        else
                            m_worldSpeechBridge->publishError(
                                m_worldSpeechCaptureId,
                                QStringLiteral("Local microphone capture "
                                               "failed."));
                        m_worldSpeechCaptureId.clear();
                    }
                    return;
                }
                startVoiceTranscription(/*finalPass=*/true);
            });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc](QProcess::ProcessError err) {
                if (err != QProcess::FailedToStart || m_voiceRecordProc != proc)
                    return;
                m_voiceRecording = false;
                m_voiceRecordProc = nullptr;
                if (m_voiceLiveTimer)
                    m_voiceLiveTimer->stop();
                stopVoiceLevelMeter();
                proc->deleteLater();
                updateVoiceInputButton();
                logSystem("Could not start the microphone recorder.");
                if (m_voiceTargetEdit)
                    m_voiceTargetEdit->setPlaceholderText(m_voiceIdlePlaceholder);
                if (m_worldSpeechBridge &&
                    m_voiceTargetEdit == m_worldSpeechDraftEdit &&
                    !m_worldSpeechCaptureId.isEmpty()) {
                    m_worldSpeechBridge->publishError(
                        m_worldSpeechCaptureId,
                        QStringLiteral("The local microphone recorder could "
                                       "not start."));
                    m_worldSpeechCaptureId.clear();
                }
            });

    proc->start(rec.program, rec.args);
    if (!proc->waitForStarted(3000)) {
        if (m_voiceRecordProc == proc) {
            m_voiceRecordProc = nullptr;
            proc->kill();
            proc->deleteLater();
            logSystem("Couldn't start the microphone recorder \xE2\x80\x94 the "
                      "audio device may be busy. Try again.");
            target->setPlaceholderText(m_voiceIdlePlaceholder);
            if (m_worldSpeechBridge &&
                target == m_worldSpeechDraftEdit &&
                !m_worldSpeechCaptureId.isEmpty()) {
                m_worldSpeechBridge->publishError(
                    m_worldSpeechCaptureId,
                    QStringLiteral("The local microphone is busy."));
                m_worldSpeechCaptureId.clear();
            }
        }
        return;
    }
    m_voiceRecording = true;
    m_voiceInsertPos = target->textCursor().position();
    m_voiceInsertLen = 0;
    m_voiceLastTranscribeSize = 0;
    m_voiceLastPreview.clear();
    if (voiceEngine() != QStringLiteral("parakeet")) {
        if (!m_voiceLiveTimer) {
            m_voiceLiveTimer = new QTimer(this);
            m_voiceLiveTimer->setInterval(1000);
            connect(m_voiceLiveTimer, &QTimer::timeout, this,
                    [this] { startVoiceTranscription(/*finalPass=*/false); });
        }
        m_voiceLiveTimer->start();
    }
    m_voiceLevelPos = 0;
    if (button == m_quickAddMicButton) {
        if (!m_voiceLevelTimer) {
            m_voiceLevelTimer = new QTimer(this);
            m_voiceLevelTimer->setInterval(80);
            connect(m_voiceLevelTimer, &QTimer::timeout, this,
                    &MainWindow::updateVoiceLevelMeter);
        }
        if (m_voiceLevelMeter) {
            m_voiceLevelMeter->setValue(0);
            m_voiceLevelMeter->setVisible(true);
        }
        m_voiceLevelTimer->start();
    }
    button->setIcon(themedOcticon("broadcast", QColor("#f85149"), 16));
    button->setIconSize(QSize(16, 16));
    button->setToolTip(
        QString::fromUtf8("Recording\xE2\x80\xA6 release to stop and transcribe."));
    target->setPlaceholderText("listening\xE2\x80\xA6 release the mic to stop");
}

void MainWindow::openVoiceSettings()
{
    showSection(1); // Settings
    if (m_settingsSections && m_voiceSettingsTabIndex >= 0)
        m_settingsSections->setCurrentIndex(m_voiceSettingsTabIndex);
}

void MainWindow::startVoiceTranscription(bool finalPass)
{
    if (!m_voiceTargetEdit)
        return;
    if (m_voiceTranscribeProc &&
        m_voiceTranscribeProc->state() != QProcess::NotRunning) {
        if (!finalPass)
            return; // a pass is already running; skip this live tick
        // code-health: allow(blocking-wait) final pass on mic release only; SIGKILL then a reap bounded at 200 ms
        m_voiceTranscribeProc->kill();
        m_voiceTranscribeProc->waitForFinished(200);
    }

    auto finishIdle = [this] {
        hideVoiceTranscribeSpinner();
        if (m_voiceTargetEdit)
            m_voiceTargetEdit->setPlaceholderText(m_voiceIdlePlaceholder);
        m_voiceInsertPos = -1;
        m_voiceInsertLen = 0;
        QFile::remove(m_voiceWavPath);
        if (m_worldSpeechBridge &&
            m_voiceTargetEdit == m_worldSpeechDraftEdit &&
            !m_worldSpeechCaptureId.isEmpty()) {
            m_worldSpeechBridge->publishFinal(m_worldSpeechCaptureId,
                                               QString());
            m_worldSpeechCaptureId.clear();
        }
    };

    const bool parakeet = voiceEngine() == QStringLiteral("parakeet");
    if (parakeet ? !parakeetInstalled()
                 : (whisperBinaryPath().isEmpty() ||
                    !QFileInfo::exists(whisperModelPath()))) {
        if (finalPass)
            finishIdle();
        return;
    }
    const qint64 wavSize =
        QFileInfo::exists(m_voiceWavPath) ? QFileInfo(m_voiceWavPath).size() : 0;
    if (wavSize < 4096) {
        if (finalPass)
            finishIdle();
        return;
    }
    if (!finalPass && wavSize - m_voiceLastTranscribeSize < 8192)
        return;
    m_voiceLastTranscribeSize = wavSize;

    if (finalPass) {
        m_voiceTargetEdit->setPlaceholderText("transcribing\xE2\x80\xA6");
        showVoiceTranscribeSpinner();
    }

    const QString base = m_voiceWavPath + QStringLiteral(".out");
    const QString wav = m_voiceWavPath;
    QFile::remove(base + QStringLiteral(".txt"));
    auto *proc = new QProcess(this);
    m_voiceTranscribeProc = proc;
    connect(proc, &QProcess::finished, this,
            [this, proc, base, wav, finalPass](int exitCode, QProcess::ExitStatus) {
                const QString err =
                    QString::fromUtf8(proc->readAllStandardError()).trimmed();
                proc->deleteLater();
                if (m_voiceTranscribeProc == proc)
                    m_voiceTranscribeProc = nullptr;
                if (m_worldSpeechCancelPending &&
                    m_voiceTargetEdit == m_worldSpeechDraftEdit) {
                    QFile::remove(base + QStringLiteral(".txt"));
                    QFile::remove(wav);
                    hideVoiceTranscribeSpinner();
                    return;
                }

                QString text;
                QFile txt(base + QStringLiteral(".txt"));
                if (txt.open(QIODevice::ReadOnly))
                    text = QString::fromUtf8(txt.readAll());
                QFile::remove(base + QStringLiteral(".txt"));
                text.remove(QStringLiteral("[BLANK_AUDIO]"));
                text = text.simplified();
                if (!text.isEmpty() && isWhisperSilenceHallucination(text) &&
                    wavPeakAmplitude(wav) < kVoiceSpokeThreshold)
                    text.clear();

                if (!finalPass) {
                    if (!text.isEmpty() && text != m_voiceLastPreview) {
                        m_voiceLastPreview = text;
                        applyVoiceTranscript(text, /*finalPass=*/false);
                    }
                    return;
                }

                hideVoiceTranscribeSpinner();
                const bool wasFooterPrompt = m_voiceTargetEdit == m_issueQuickAdd;
                if (m_voiceTargetEdit)
                    m_voiceTargetEdit->setPlaceholderText(m_voiceIdlePlaceholder);
                QFile::remove(wav);
                if (text.isEmpty() && exitCode != 0 &&
                    m_worldSpeechBridge &&
                    m_voiceTargetEdit == m_worldSpeechDraftEdit &&
                    !m_worldSpeechCaptureId.isEmpty()) {
                    m_worldSpeechBridge->publishError(
                        m_worldSpeechCaptureId,
                        QStringLiteral("Local speech-to-text failed."));
                    m_worldSpeechCaptureId.clear();
                }
                applyVoiceTranscript(text, /*finalPass=*/true);
                if (text.isEmpty() && exitCode != 0)
                    logSystem("Transcription failed" +
                              (err.isEmpty() ? QString() : ": " + err.right(200)));
                else if (text.isEmpty())
                    logSystem("No speech detected \xE2\x80\x94 check that your "
                              "microphone is capturing audio.");
                else if (wasFooterPrompt &&
                         QSettings()
                             .value(kVoiceAutoSubmitSetting, false)
                             .toBool())
                    quickAddIssue();
            });
    if (parakeet) {
        proc->start(parakeetPython(),
                    {parakeetScriptPath(), wav, base + QStringLiteral(".txt"),
                     parakeetModelName()});
    } else {
        const int threads = qBound(2, QThread::idealThreadCount() - 1, 8);
        proc->start(whisperBinaryPath(),
                    {QStringLiteral("-m"), whisperModelPath(), QStringLiteral("-f"),
                     wav, QStringLiteral("-nt"), QStringLiteral("-otxt"),
                     QStringLiteral("-of"), base, QStringLiteral("-bs"),
                     QStringLiteral("1"), QStringLiteral("-nf"), QStringLiteral("-t"),
                     QString::number(threads)});
    }
}

void MainWindow::applyVoiceTranscript(const QString &text, bool finalPass)
{
    if (!m_voiceTargetEdit || m_voiceInsertPos < 0) {
        if (finalPass && m_worldSpeechBridge &&
            m_voiceTargetEdit == m_worldSpeechDraftEdit &&
            !m_worldSpeechCaptureId.isEmpty()) {
            m_worldSpeechBridge->publishFinal(m_worldSpeechCaptureId, text);
            m_worldSpeechCaptureId.clear();
        }
        if (finalPass) {
            m_voiceInsertPos = -1;
            m_voiceInsertLen = 0;
        }
        return;
    }
    const QString full = m_voiceTargetEdit->toPlainText();
    const int start = qBound(0, m_voiceInsertPos, full.size());
    const int end = qBound(start, start + m_voiceInsertLen, full.size());

    QString prefix;
    if (start > 0 && start <= full.size() && !full.at(start - 1).isSpace())
        prefix = QStringLiteral(" ");
    const QString ins = text.isEmpty() ? QString() : prefix + text;

    QTextCursor cur = m_voiceTargetEdit->textCursor();
    cur.setPosition(start);
    cur.setPosition(end, QTextCursor::KeepAnchor);
    cur.insertText(ins);
    m_voiceInsertLen = ins.size();

    if (m_worldSpeechBridge &&
        m_voiceTargetEdit == m_worldSpeechDraftEdit &&
        !m_worldSpeechCaptureId.isEmpty()) {
        if (finalPass) {
            m_worldSpeechBridge->publishFinal(m_worldSpeechCaptureId, text);
            m_worldSpeechCaptureId.clear();
            if (m_worldSpeechStatusLabel)
                m_worldSpeechStatusLabel->setText(
                    QStringLiteral("World transcript completed locally. "
                                   "Review it in the selected browser "
                                   "composer."));
        } else {
            m_worldSpeechBridge->publishPartial(m_worldSpeechCaptureId, text);
        }
    }

    if (finalPass) {
        m_voiceInsertPos = -1;
        m_voiceInsertLen = 0;
        m_voiceTargetEdit->setTextCursor(cur);
        if (m_voiceTargetEdit != m_worldSpeechDraftEdit)
            m_voiceTargetEdit->setFocus();
    }
}

void MainWindow::updateVoiceLevelMeter()
{
    if (!m_voiceLevelMeter)
        return;
    const double peak = wavLevelSince(m_voiceWavPath, &m_voiceLevelPos);
    const int cur = m_voiceLevelMeter->value();
    int next;
    if (peak < 0.0) {
        next = qMax(0, cur - 14); // no new audio: decay toward silence
    } else {
        const int target =
            int(qBound(0.0, qSqrt(peak) * 135.0, 100.0));
        next = target >= cur ? target : qMax(target, cur - 14);
    }
    if (next != cur)
        m_voiceLevelMeter->setValue(next);
}

void MainWindow::stopVoiceLevelMeter()
{
    if (m_voiceLevelTimer)
        m_voiceLevelTimer->stop();
    if (m_voiceLevelMeter) {
        m_voiceLevelMeter->setValue(0);
        m_voiceLevelMeter->setVisible(false);
    }
}

void MainWindow::showVoiceTranscribeSpinner()
{
    QPushButton *btn = m_voiceActiveButton;
    if (!btn || !btn->parentWidget())
        return;
    if (!m_voiceTranscribeSpinner)
        m_voiceTranscribeSpinner = new RingSpinner(btn->parentWidget());
    QWidget *sp = m_voiceTranscribeSpinner;
    if (sp->parentWidget() != btn->parentWidget())
        sp->setParent(btn->parentWidget());
    const QRect bg = btn->geometry();
    const int pad = 3;
    const int d = qMax(bg.width(), bg.height()) + 2 * pad;
    QRect r(0, 0, d, d);
    r.moveCenter(bg.center());
    sp->setGeometry(r);
    sp->raise();
    sp->show();
}

void MainWindow::hideVoiceTranscribeSpinner()
{
    if (m_voiceTranscribeSpinner)
        m_voiceTranscribeSpinner->hide();
}
