#include "MarketingFeature.h"
#include "MarketingCommon.h"
#include "MarketingDashboard.h"
#include "PackageRailTile.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QIntValidator>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <tuple>
#include <utility>

namespace forkmesh::marketing {
namespace {

QString adLabel(const QString &value)
{
    QString label = channelName(value);
    return label.replace(QLatin1Char('-'), QLatin1Char(' '));
}

QString adPacingText(const QString &state)
{
    if (state == QLatin1String("on-track"))
        return QStringLiteral("On pace");
    if (state == QLatin1String("over"))
        return QStringLiteral("Spending fast");
    if (state == QLatin1String("under"))
        return QStringLiteral("Spending slow");
    if (state == QLatin1String("overspent"))
        return QStringLiteral("Over budget");
    return QString::fromUtf8("\xE2\x80\x94");
}

QString adErrorText(const QJsonObject &payload, const QString &fallback)
{
    const QString code = text(payload, "error");
    if (code == QLatin1String("ad_name_required"))
        return QStringLiteral("give the ad a name");
    if (code == QLatin1String("invalid_ad_url"))
        return QStringLiteral("the landing page and console links must be https:// addresses");
    if (code == QLatin1String("invalid_ad_currency"))
        return QStringLiteral("the currency must be a three-letter code such as USD");
    if (code == QLatin1String("invalid_ad_dates"))
        return QStringLiteral("the end date is before the start date");
    if (code == QLatin1String("invalid_ad_channel"))
        return QStringLiteral("choose a channel from the catalog");
    return fallback;
}

constexpr char kMarketingPage[] = "https://app.forkmesh.com/dashboard/marketing";
constexpr qint64 kDiscordPostsFreshMs = 60 * 1000;

QString engagementFormula(const QJsonObject &health)
{
    const QJsonObject basis = health.value(QStringLiteral("engagement")).toObject();
    if (basis.isEmpty())
        return QStringLiteral("No engagement rate: this channel reported no engagements, "
                              "or nothing to divide them by.");
    QString note = QStringLiteral("Engagement rate = %1.").arg(text(basis, "formula"));
    if (basis.value(QStringLiteral("readings")).toInt() > 1) {
        note += QStringLiteral(" The same two numbers have come back since %1 (%2 readings), "
                               "so they may not be refreshing.")
                    .arg(shortDate(qint64(number(basis, "unchangedSince"))))
                    .arg(basis.value(QStringLiteral("readings")).toInt());
    }
    return note;
}

QString engagementBaseLabel(const QJsonObject &health)
{
    const QJsonObject basis = health.value(QStringLiteral("engagement")).toObject();
    const QString label = text(basis, "baseLabel");
    return label.isEmpty() ? QStringLiteral("of reach") : QStringLiteral("of %1").arg(label);
}

QString sourceLabel(const QString &source)
{
    if (source == QLatin1String("extension") || source == QLatin1String("x-profile"))
        return QStringLiteral("browser extension");
    if (source == QLatin1String("crm"))
        return QStringLiteral("CRM");
    if (source == QLatin1String("zoho"))
        return QStringLiteral("CRM and Zoho Mail");
    if (source == QLatin1String("refresh"))
        return QStringLiteral("channel refresh");
    return source.isEmpty() ? QStringLiteral("logged") : source;
}

QString channelCategory(const QString &channel, const QJsonArray &catalog)
{
    for (const QJsonValue &value : catalog) {
        const QJsonObject item = value.toObject();
        if (text(item, "id") == channel)
            return text(item, "category");
    }
    return QString();
}

QString normalizedChannelUrl(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty() || trimmed.contains(QStringLiteral("://")) ||
        trimmed.startsWith(QStringLiteral("http:"), Qt::CaseInsensitive))
        return trimmed;
    QString path = trimmed;
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    return QStringLiteral("https://") + path;
}

QString channelOpenUrl(const QJsonObject &settings)
{
    const QString open = normalizedChannelUrl(text(settings, "openUrl"));
    return open.startsWith(QLatin1String("https://")) ? open : normalizedChannelUrl(text(settings, "profileUrl"));
}

QString channelUrlPlaceholder(const QString &id)
{
    static const QHash<QString, QString> examples{
        {QStringLiteral("blog"), QStringLiteral("https://forkmesh.com/blog")},
        {QStringLiteral("reddit"), QStringLiteral("https://www.reddit.com/r/name")},
        {QStringLiteral("x"), QStringLiteral("https://x.com/name")},
        {QStringLiteral("youtube"), QStringLiteral("https://www.youtube.com/@name")},
        {QStringLiteral("mastodon"), QStringLiteral("https://mastodon.social/@name")},
        {QStringLiteral("discord"), QStringLiteral("https://discord.gg/invite")},
        {QStringLiteral("github"), QStringLiteral("https://github.com/name")},
    };
    return examples.value(id, QStringLiteral("https://public-profile"));
}

QString refreshErrorText(const QString &id, const QString &code, const QString &fallback)
{
    if (code == QLatin1String("channel_url_required"))
        return QStringLiteral("add a channel URL first");
    if (code == QLatin1String("channel_disabled"))
        return QStringLiteral("the channel is paused");
    if (code == QLatin1String("channel_refresh_unsupported"))
        return QStringLiteral("automatic refresh isn't available for this channel yet; use Add metrics");
    if (code == QLatin1String("outbound_origin_not_allowed"))
        return id == QLatin1String("mastodon")
            ? QStringLiteral("that Mastodon server isn't approved yet (mastodon.social, mastodon.online, fosstodon.org, hachyderm.io, mas.to, infosec.exchange)")
            : QStringLiteral("that site isn't on the approved list");
    if (code == QLatin1String("outbound_redirect_rejected"))
        return id == QLatin1String("youtube")
            ? QStringLiteral("YouTube redirected the request; use the https://www.youtube.com/channel/UC… URL")
            : QStringLiteral("the site redirected; use the final https:// URL");
    if (code == QLatin1String("analytics_not_installed"))
        return QStringLiteral("install Analytics to count visits from Google");
    if (code == QLatin1String("crm_not_installed"))
        return QStringLiteral("install CRM for this organization to count outbound email and closed deals");
    if (code.startsWith(QLatin1String("crm_")))
        return QStringLiteral("CRM could not be read just now");
    if (code == QLatin1String("analytics_no_project"))
        return QStringLiteral("create an Analytics project for your site to count visits from Google");
    if (code.startsWith(QLatin1String("analytics_")))
        return QStringLiteral("Analytics could not be read just now");
    if (code == QLatin1String("x_access_token_required"))
        return QStringLiteral("an X API bearer token is required");
    if (code == QLatin1String("x_access_token_invalid"))
        return QStringLiteral("X rejected the API token");
    if (code == QLatin1String("mastodon_access_token_required"))
        return QStringLiteral("add a Mastodon access token to follow from here");
    if (code == QLatin1String("mastodon_access_token_invalid"))
        return QStringLiteral("Mastodon rejected the access token; it needs the read and write:follows scopes");
    if (code == QLatin1String("x_rate_limited") || code == QLatin1String("github_rate_limited") ||
        code == QLatin1String("mastodon_rate_limited"))
        return QStringLiteral("the API rate limit was reached; try again later or add a token");
    if (code == QLatin1String("reddit_rate_limited"))
        return QStringLiteral("Reddit is rate limiting ForkMesh's servers; the next Refresh waits 15 minutes, and Update reads the counts from your own browser meanwhile");
    if (code == QLatin1String("reddit_refused"))
        return QStringLiteral("Reddit refused the feed from ForkMesh's servers; use Update to read the page in your own browser");
    if (code == QLatin1String("reddit_not_found"))
        return QStringLiteral("Reddit has no such subreddit or profile; check the channel link");
    if (code == QLatin1String("reddit_feed_invalid"))
        return QStringLiteral("Reddit answered with something that is not a feed; try again later");
    if (code.startsWith(QLatin1String("channel_http_")))
        return QStringLiteral("the site answered %1").arg(code.mid(13));
    if (code == QLatin1String("discord_secret_required"))
        return QStringLiteral("the Discord connector has no bot token yet");
    if (code == QLatin1String("discord_authorization_required"))
        return QStringLiteral("the Discord bot lost access; reconnect it in the Discord connector");
    if (code == QLatin1String("discord_guild_missing"))
        return QStringLiteral("the Discord bot is no longer in the server; add it again");
    if (code == QLatin1String("discord_rate_limited"))
        return QStringLiteral("Discord is rate limiting the bot; try again later");
    if (code == QLatin1String("discord_connector_not_connected"))
        return QStringLiteral("connect Discord in the organization's settings and choose its channels");
    if (code == QLatin1String("discord_connector_not_allowed"))
        return QStringLiteral("update Marketing so it may read the Discord connector");
    if (code == QLatin1String("discord_channel_required"))
        return QStringLiteral("choose the channels to read in the organization's Discord settings");
    if (code == QLatin1String("organization_member_required"))
        return QStringLiteral("only organization members can read the Discord posts");
    if (code == QLatin1String("discord_widget_disabled"))
        return QStringLiteral("enable the server widget or use an invite link");
    if (code == QLatin1String("discord_invite_invalid"))
        return QStringLiteral("the invite link is invalid or expired");
    if (code.endsWith(QLatin1String("_url_unsupported")))
        return QStringLiteral("use a URL like %1").arg(channelUrlPlaceholder(id));
    if (code.endsWith(QLatin1String("_not_found")))
        return QStringLiteral("the account could not be found");
    return fallback;
}

// One small bar per time bucket: how many posts went out when. A single
// series, so one accent (dataviz reference slot 1) and no legend; the row's
// channel name labels it and the tooltip carries the exact bucket and count.
class VelocityChart final : public QWidget
{
public:
    explicit VelocityChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(210, 26);
        setMouseTracking(true);
    }

    void setSeries(const QString &name, QVector<int> counts, qint64 start,
                   qint64 bucketMs, const QString &summary)
    {
        m_name = name;
        m_counts = std::move(counts);
        m_start = start;
        m_bucketMs = bucketMs;
        m_summary = summary;
        setAccessibleName(name + QStringLiteral(" posting velocity"));
        setAccessibleDescription(summary);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor accent(dark ? QStringLiteral("#3987e5") : QStringLiteral("#2a78d6"));
        QColor muted = palette().color(QPalette::PlaceholderText);
        const QRectF plot = plotRect();

        QColor baseline = muted;
        baseline.setAlphaF(0.45);
        painter.setPen(QPen(baseline, 1));
        painter.drawLine(QPointF(plot.left(), plot.bottom() + 0.5),
                         QPointF(plot.right(), plot.bottom() + 0.5));

        const int peak = m_counts.isEmpty() ? 0
            : *std::max_element(m_counts.cbegin(), m_counts.cend());
        if (peak > 0) {
            const qreal slot = plot.width() / m_counts.size();
            const qreal gap = slot >= 5 ? 2 : 1;
            const qreal barWidth = qMax<qreal>(1, slot - gap);
            const qreal radius = qMin<qreal>(4, barWidth / 2);
            painter.setPen(Qt::NoPen);
            for (int index = 0; index < m_counts.size(); ++index) {
                const QRectF column(plot.left() + index * slot, plot.top(), slot, plot.height());
                if (index == m_hover) {
                    QColor band = muted;
                    band.setAlphaF(0.18);
                    painter.fillRect(column, band);
                }
                if (m_counts.at(index) <= 0)
                    continue;
                const qreal height = qMax<qreal>(2, plot.height() * m_counts.at(index) / peak);
                const QRectF bar(column.left() + gap / 2, plot.bottom() - height,
                                 barWidth, height + radius);
                // Round the data end only; the extra radius below the
                // baseline is clipped so the bar sits flat on it.
                painter.save();
                painter.setClipRect(QRectF(column.left(), plot.top(), slot, plot.height()));
                painter.setBrush(accent);
                painter.drawRoundedRect(bar, radius, radius);
                painter.restore();
            }
        }

        const QRectF label(plot.right() + 8, 0, width() - plot.right() - 8, height());
        painter.setPen(peak > 0 ? palette().color(QPalette::Text) : muted);
        painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(m_summary, Qt::ElideRight,
                                                  int(label.width())));
    }

    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip)
            return QWidget::event(event);
        const auto *help = static_cast<QHelpEvent *>(event);
        const int index = bucketAt(help->pos().x());
        if (index < 0) {
            QToolTip::showText(help->globalPos(), m_name + QLatin1Char('\n') + m_summary, this);
            return true;
        }
        const QDateTime from = QDateTime::fromMSecsSinceEpoch(m_start + index * m_bucketMs);
        const QDateTime to = QDateTime::fromMSecsSinceEpoch(m_start + (index + 1) * m_bucketMs);
        const QString range = m_bucketMs >= 24 * 3600 * 1000LL
            ? from.toString(QStringLiteral("ddd MMM d"))
            : from.toString(QStringLiteral("MMM d HH:mm")) + QStringLiteral(" – ") +
                  to.toString(QStringLiteral("HH:mm"));
        const int count = m_counts.at(index);
        QToolTip::showText(help->globalPos(),
                           QStringLiteral("%1\n%2\n%3 post%4").arg(m_name, range)
                               .arg(count).arg(count == 1 ? QString() : QStringLiteral("s")),
                           this);
        return true;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int index = bucketAt(event->position().x());
        if (index != m_hover) {
            m_hover = index;
            update();
        }
    }

    void leaveEvent(QEvent *event) override
    {
        m_hover = -1;
        update();
        QWidget::leaveEvent(event);
    }

private:
    QRectF plotRect() const
    {
        const qreal labelWidth = qMin<qreal>(118, width() * 0.45);
        return QRectF(4, 4, qMax<qreal>(10, width() - labelWidth - 12), height() - 9);
    }

    int bucketAt(qreal x) const
    {
        const QRectF plot = plotRect();
        if (m_counts.isEmpty() || x < plot.left() || x >= plot.right())
            return -1;
        return qBound(0, int((x - plot.left()) / (plot.width() / m_counts.size())),
                      int(m_counts.size()) - 1);
    }

    QString m_name;
    QString m_summary;
    QVector<int> m_counts;
    qint64 m_start = 0;
    qint64 m_bucketMs = 0;
    int m_hover = -1;
};


QString agoText(qint64 days)
{
    if (days <= 0)
        return QStringLiteral("today");
    if (days == 1)
        return QStringLiteral("yesterday");
    return QStringLiteral("%1 days ago").arg(days);
}

forkmesh::market::NativePackageRailTone railTone(const QString &state)
{
    using Tone = forkmesh::market::NativePackageRailTone;
    if (state == QLatin1String("good"))
        return Tone::Good;
    if (state == QLatin1String("attention"))
        return Tone::Attention;
    if (state == QLatin1String("stale"))
        return Tone::Warning;
    if (state == QLatin1String("paused"))
        return Tone::Paused;
    return Tone::Neutral;
}

QString chipToneSheet(const QString &state, bool paused, const DashboardColors &colors)
{
    const QColor tone = forkmesh::market::packageRailToneColor(
        paused ? forkmesh::market::NativePackageRailTone::Paused : railTone(state), colors.dark);
    QColor fill = tone;
    fill.setAlphaF(colors.dark ? 0.20 : 0.12);
    QColor hover = tone;
    hover.setAlphaF(fill.alphaF() + 0.12);
    return QStringLiteral(
        "QFrame#marketingChannelChip { background: %1; border: 1px %3 %2; border-radius: 8px; }"
        "QFrame#marketingChannelChip:hover, QFrame#marketingChannelChip:focus { background: %4; }"
        "QFrame#marketingChannelChip[current=\"true\"] { background: %4; border: 2px solid %5; }")
        .arg(fill.name(QColor::HexArgb), tone.name(QColor::HexArgb),
             paused ? QStringLiteral("dashed") : QStringLiteral("solid"),
             hover.name(QColor::HexArgb), colors.primary.name(QColor::HexArgb));
}

constexpr int kXLimit = 280;

QStringList contentStatuses()
{
    return {QStringLiteral("draft"), QStringLiteral("review"), QStringLiteral("queued"),
            QStringLiteral("scheduled"), QStringLiteral("published"), QStringLiteral("archived")};
}

const Choices &campaignStatuses()
{
    static const Choices choices{
        {QStringLiteral("idea"), QStringLiteral("Idea")},
        {QStringLiteral("planned"), QStringLiteral("Planned")},
        {QStringLiteral("active"), QStringLiteral("Active")},
        {QStringLiteral("paused"), QStringLiteral("Paused")},
        {QStringLiteral("completed"), QStringLiteral("Completed")},
        {QStringLiteral("archived"), QStringLiteral("Archived")}};
    return choices;
}

const Choices &campaignObjectives()
{
    static const Choices choices{
        {QStringLiteral("awareness"), QStringLiteral("Awareness")},
        {QStringLiteral("traffic"), QStringLiteral("Traffic")},
        {QStringLiteral("signups"), QStringLiteral("Signups")},
        {QStringLiteral("activation"), QStringLiteral("Activation")},
        {QStringLiteral("engagement"), QStringLiteral("Engagement")},
        {QStringLiteral("launch"), QStringLiteral("Launch")},
        {QStringLiteral("community"), QStringLiteral("Community")},
        {QStringLiteral("sales"), QStringLiteral("Sales")}};
    return choices;
}

bool isOpenCampaign(const QString &status)
{
    return status == QLatin1String("idea") || status == QLatin1String("planned") ||
           status == QLatin1String("active") || status == QLatin1String("paused");
}

QString durationText(double ms)
{
    const qint64 seconds = std::llround(ms / 1000);
    return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

using MetricSeries = QHash<QString, QList<QJsonObject>>;

QString pageKey(const QString &url)
{
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.host().isEmpty())
        return QString();
    QString host = parsed.host().toLower();
    if (host.startsWith(QLatin1String("www.")))
        host = host.mid(4);
    QString path = parsed.path(QUrl::FullyEncoded);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return host + (path.isEmpty() ? QStringLiteral("/") : path);
}

QVector<double> dailyViews(const QJsonArray &days, qint64 start, qint64 end, int buckets)
{
    const qint64 span = qMax<qint64>(1, end - start);
    QVector<double> counts(qMax(1, buckets), 0);
    for (const QJsonValue &value : days) {
        const QJsonArray day = value.toArray();
        const qint64 at = qint64(day.at(0).toDouble()) + kDay / 2;
        if (at < start || at > end)
            continue;
        counts[qMin(int(counts.size()) - 1, int((at - start) * counts.size() / span))] += day.at(1).toDouble();
    }
    return counts;
}

MetricSeries seriesByChannel(const QJsonArray &metrics)
{
    MetricSeries series;
    for (const QJsonValue &value : metrics) {
        const QJsonObject metric = value.toObject();
        series[text(metric, "channel")].append(metric);
    }
    for (auto it = series.begin(); it != series.end(); ++it) {
        std::stable_sort(it->begin(), it->end(), [](const QJsonObject &a, const QJsonObject &b) {
            return number(a, "observedAt") < number(b, "observedAt");
        });
    }
    return series;
}

QJsonObject snapshotAt(const QList<QJsonObject> &list, qint64 at)
{
    QJsonObject found;
    for (const QJsonObject &item : list) {
        if (number(item, "observedAt") > at)
            break;
        found = item;
    }
    return found;
}

struct MetricTotals {
    double reach = 0;
    double engagements = 0;
    double followers = 0;
};

MetricTotals totalsAt(const MetricSeries &series, qint64 at, const QSet<QString> *only = nullptr)
{
    MetricTotals totals;
    for (auto it = series.cbegin(); it != series.cend(); ++it) {
        if (only && !only->contains(it.key()))
            continue;
        const QJsonObject snapshot = snapshotAt(it.value(), at);
        totals.reach += number(snapshot, "reach");
        totals.engagements += number(snapshot, "engagements");
        totals.followers += number(snapshot, "followers");
    }
    return totals;
}

QList<QPair<QString, QColor>> channelColors(const MetricSeries &series, const DashboardColors &colors)
{
    QList<QPair<QString, double>> ranked;
    for (auto it = series.cbegin(); it != series.cend(); ++it)
        ranked.append({it.key(), it.value().isEmpty() ? 0 : number(it.value().constLast(), "reach")});
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    QList<QPair<QString, QColor>> result;
    for (int index = 0; index < ranked.size() && index < 4; ++index)
        result.append({ranked.at(index).first, colors.series[index]});
    return result;
}

QPair<QString, int> percentDelta(double current, double previous)
{
    if (previous <= 0)
        return {current > 0 ? QStringLiteral("New") : kDash, 0};
    const double change = (current - previous) / previous * 100;
    const int tone = change > 0.05 ? 1 : change < -0.05 ? -1 : 0;
    return {QStringLiteral("%1 %2%")
                .arg(tone > 0 ? QStringLiteral("▲") : tone < 0 ? QStringLiteral("▼") : QStringLiteral("•"))
                .arg(std::abs(change), 0, 'f', 1),
            tone};
}

struct Headline {
    double value = 0;
    QString label;
    const char *key = nullptr;
};

Headline headline(const QString &id, const QJsonObject &item, const QJsonObject &health)
{
    if (id == QLatin1String("email"))
        return {number(item, "outbound"), QStringLiteral("Emails sent · 30D"), "outbound"};
    if (id == QLatin1String("github") && number(item, "stars") > 0)
        return {number(item, "stars"), QStringLiteral("Stars"), "stars"};
    if (number(item, "followers") > 0)
        return {number(item, "followers"),
                id == QLatin1String("youtube") ? QStringLiteral("Subscribers")
                : id == QLatin1String("reddit") ? QStringLiteral("Members")
                                                : QStringLiteral("Followers"),
                "followers"};
    if (number(item, "views") > 0)
        return {number(item, "views"), QStringLiteral("Views"), "views"};
    if (number(item, "visits") > 0)
        return {number(item, "visits"), QStringLiteral("Visits · 30D"), "visits"};
    if (number(item, "indexedPages") > 0)
        return {number(item, "indexedPages"), QStringLiteral("Pages on Google"), "indexedPages"};
    if (number(item, "reach") > 0)
        return {number(item, "reach"), QStringLiteral("Reach"), "reach"};
    return {number(health, "postsLast30d"), QStringLiteral("Posts · 30D"), nullptr};
}

constexpr const char *kContextKeys[] = {"followers", "following", "reach", "engagements", "views", "visits",
                                        "posts", "likes", "replies", "stars", "forks", "outbound",
                                        "closedDeals", "indexedPages", "reviews"};
constexpr const char *kContextLabels[] = {"Followers", "Following", "Reach", "Engagements", "Views", "Visits",
                                          "Posts", "Likes", "Replies", "Stars", "Forks", "Emails sent",
                                          "Closed deals", "Pages on Google", "Reviews"};

QString contextKey(const QString &organization, const QString &channel)
{
    return QStringLiteral("marketing/aiContext/%1/%2").arg(organization, channel);
}

QVariantMap contextNumbers(const QJsonObject &item)
{
    QVariantMap values;
    for (const char *key : kContextKeys) {
        if (number(item, key) > 0)
            values.insert(QLatin1String(key), number(item, key));
    }
    values.insert(QStringLiteral("postsLast30d"),
                  number(item.value(QStringLiteral("health")).toObject(), "postsLast30d"));
    return values;
}

QIcon dotIcon(const QColor &color)
{
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(3, 3, 14, 14), 4, 4);
    return QIcon(pixmap);
}

int xLength(const QString &value)
{
    static const QRegularExpression links(QStringLiteral("https?://\\S+"));
    QString counted = value;
    counted.replace(links, QString(23, QLatin1Char('x')));
    return int(counted.toUcs4().size());
}

QList<QJsonObject> agentsOnChannel(const QJsonArray &rows, const QString &channel)
{
    QList<QJsonObject> agents;
    for (const QJsonValue &value : rows) {
        const QJsonObject agent = value.toObject();
        const QJsonArray channels = agent.value(QStringLiteral("marketingChannels")).toArray();
        if (std::any_of(channels.cbegin(), channels.cend(),
                        [&channel](const QJsonValue &listed) { return listed.toString() == channel; }))
            agents.append(agent);
    }
    return agents;
}

QLabel *agentFace(const QJsonObject &agent, int side, const DashboardColors &colors, QWidget *owner)
{
    auto *face = new QLabel;
    face->setObjectName(QStringLiteral("marketingAgentFace"));
    face->setFixedSize(side, side);
    face->setAlignment(Qt::AlignCenter);
    QPixmap art;
    if (art.loadFromData(QByteArray::fromBase64(agent.value(QStringLiteral("face")).toString().toLatin1()))) {
        const qreal dpr = owner ? owner->devicePixelRatioF() : 1.0;
        art = art.scaled(qRound(side * dpr), qRound(side * dpr), Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation);
        art.setDevicePixelRatio(dpr);
        face->setPixmap(art);
        return face;
    }
    face->setText(initials(text(agent, "name")));
    face->setStyleSheet(QStringLiteral("background: %1; color: #ffffff; border-radius: %2px; "
                                       "font-size: %3px; font-weight: 600;")
                            .arg(colors.primary.name()).arg(side / 2).arg(qMax(7, side * 3 / 8)));
    return face;
}

bool agentBusy(const QJsonObject &agent)
{
    const QString status = text(agent, "status");
    return status == QLatin1String("Working") || status == QLatin1String("Queued") ||
           status == QLatin1String("Waiting");
}

bool canWriteReadyPost(const QString &role)
{
    return role == QLatin1String("owner") || role == QLatin1String("admin") ||
           role == QLatin1String("maintainer") || role == QLatin1String("member");
}

QString readyPostTask(const QJsonObject &ready)
{
    const QJsonArray history = ready.value(QStringLiteral("history")).toArray();
    const QString status = history.isEmpty() ? QString() : text(history.at(0).toObject(), "status");
    if (status == QLatin1String("posted"))
        return QStringLiteral("The ready X post was posted. Write the next one.");
    if (status == QLatin1String("skipped"))
        return QStringLiteral("The ready X post was skipped. Write the next one.");
    return QStringLiteral("There is no ready X post. Write the next one.");
}

QString readyPostErrorText(const QString &code, const QString &fallback)
{
    if (code == QLatin1String("organization_member_required"))
        return QStringLiteral("only organization members can see the ready post");
    if (code == QLatin1String("invalid_ready_post_status"))
        return QStringLiteral("Marketing did not understand the change; update the desktop");
    return fallback;
}

constexpr qint64 kReadyPostRunGap = 30 * 60 * 1000LL;

} // namespace

MarketingFeature::MarketingFeature(
    const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_packageId(host.packageId),
      m_searchIndexChanged(host.searchIndexChanged),
      m_railChanged(host.railChanged),
      m_specialAgents(host.specialAgents),
      m_specialAgentAction(host.specialAgentAction),
      m_network(host.network)
{
    setObjectName(QStringLiteral("marketingSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 10, 12, 0);
    outer->setSpacing(6);

    m_heading = new QHBoxLayout;
    auto *heading = m_heading;
    heading->setSpacing(8);
    m_organization = new QComboBox;
    m_organization->setMinimumWidth(140);
    m_organization->setAccessibleName(QStringLiteral("Organization"));
    heading->addWidget(m_organization);
    m_rangeGroup = new QButtonGroup(this);
    m_rangeSwitch = segmented({QStringLiteral("7D"), QStringLiteral("30D"), QStringLiteral("90D")},
                              m_rangeGroup, 1, QStringLiteral("Date range"));
    m_metricButton = new QPushButton(QStringLiteral("Add metrics"));
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    refreshButton->setToolTip(QStringLiteral("Refresh every active channel that has a URL"));
    m_webButton = new QPushButton(QStringLiteral("Open on web ↗"));
    m_webButton->setToolTip(QStringLiteral("Open this page of Marketing in the browser"));
    m_newButton = new QPushButton(QStringLiteral("New content"));
    m_newButton->setObjectName(QStringLiteral("primaryButton"));
    for (QPushButton *button : {m_metricButton, refreshButton, m_webButton})
        button->setObjectName(QStringLiteral("ghostButton"));
    heading->addWidget(refreshButton);
    heading->addWidget(m_webButton);
    outer->addLayout(heading);

    m_status = new QLabel(QStringLiteral("Select an organization to load Marketing."));
    m_status->setObjectName(QStringLiteral("mutedLabel"));
    m_status->setWordWrap(true);
    outer->addWidget(m_status);

    m_browserActivity = new QLabel;
    m_browserActivity->setObjectName(QStringLiteral("marketingBrowserActivity"));
    m_browserActivity->setAccessibleName(QStringLiteral("Browser activity"));
    m_browserActivity->setTextFormat(Qt::RichText);
    m_browserActivity->setWordWrap(true);
    m_browserActivity->setOpenExternalLinks(true);
    m_browserActivity->hide();
    outer->addWidget(m_browserActivity);

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    m_installNote = new QLabel;
    m_installNote->setAlignment(Qt::AlignHCenter);
    m_installNote->setWordWrap(true);
    installLayout->addWidget(m_installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch();
    m_installButton = new QPushButton(QStringLiteral("Install for this organization"));
    m_installButton->setObjectName(QStringLiteral("primaryButton"));
    installRow->addWidget(m_installButton);
    installRow->addStretch();
    installLayout->addLayout(installRow);
    installLayout->addStretch();
    outer->addWidget(m_installPanel, 1);

    m_scroll = new QScrollArea;
    m_scroll->setObjectName(QStringLiteral("marketingDashboard"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("marketingDashboardPage"));
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 2, 6, 12);
    pageLayout->setSpacing(8);
    m_kpis = new KpiStrip;
    m_kpis->setCompact(true);
    pageLayout->addWidget(m_kpis);
    auto *grid = new DashboardGrid;
    static_cast<QGridLayout *>(grid->layout())->setSpacing(8);
    pageLayout->addWidget(grid);
    pageLayout->addStretch(1);
    m_scroll->setWidget(page);
    outer->addWidget(m_scroll, 1);

    m_detailScroll = new QScrollArea;
    m_detailScroll->setObjectName(QStringLiteral("marketingChannelPage"));
    m_detailScroll->setFrameShape(QFrame::NoFrame);
    m_detailScroll->setWidgetResizable(true);
    m_detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *detailPage = new QWidget;
    detailPage->setObjectName(QStringLiteral("marketingDashboardPage"));
    auto *detailLayout = new QVBoxLayout(detailPage);
    detailLayout->setContentsMargins(0, 2, 6, 14);
    detailLayout->setSpacing(10);
    m_detailHead = new QFrame;
    m_detailHead->setObjectName(QStringLiteral("marketingPanel"));
    auto *detailHeadLayout = new QVBoxLayout(m_detailHead);
    detailHeadLayout->setContentsMargins(12, 9, 12, 10);
    detailHeadLayout->setSpacing(6);
    m_detailKpis = new KpiStrip;
    m_detailKpis->setCompact(true);
    detailLayout->addWidget(m_detailKpis);
    auto *detailGrid = new DashboardGrid;
    static_cast<QGridLayout *>(detailGrid->layout())->setSpacing(10);
    detailLayout->addWidget(detailGrid);
    detailLayout->addStretch(1);
    m_detailScroll->setWidget(detailPage);
    outer->addWidget(m_detailScroll, 1);
    const auto detailBox = [detailGrid](const QString &title, int span, DashboardPanel **out = nullptr) {
        auto *panel = new DashboardPanel(title);
        panel->setCompact();
        auto *box = new QVBoxLayout;
        box->setSpacing(4);
        panel->body()->addLayout(box);
        detailGrid->addPanel(panel, span);
        if (out)
            *out = panel;
        return box;
    };
    const auto sideBox = [](QBoxLayout *column, const QString &title, DashboardPanel **out) {
        auto *panel = new DashboardPanel(title);
        panel->setCompact();
        auto *box = new QVBoxLayout;
        box->setSpacing(4);
        panel->body()->addLayout(box);
        column->addWidget(panel);
        *out = panel;
        return box;
    };

    m_detailAgent = detailBox(QStringLiteral("Special agent"), 3, &m_detailAgentPanel);
    m_detailAgentPanel->setObjectName(QStringLiteral("marketingChannelAgentPanel"));
    m_detailAgentPanel->hide();

    m_detailChartPanel = new DashboardPanel(QStringLiteral("Followers over time"));
    m_detailChartPanel->setCompact();
    m_detailMetric = new QButtonGroup(this);
    m_detailChartPanel->tools()->addWidget(segmented(
        {QStringLiteral("Followers"), QStringLiteral("Reach"), QStringLiteral("Engagements"),
         QStringLiteral("Views"), QStringLiteral("Stars"), QStringLiteral("Forks"), QStringLiteral("Following")},
        m_detailMetric, 0, QStringLiteral("Channel chart metric")));
    m_detailChart = new LineChart;
    m_detailChart->setAccessibleName(QStringLiteral("Channel history chart"));
    m_detailChart->setMaximumHeight(210);
    m_detailChart->setMinimumWidth(240);
    m_detailChartPanel->body()->addWidget(m_detailChart);
    m_detailViewsPanel = new DashboardPanel(QStringLiteral("Views over time"));
    m_detailViewsPanel->setCompact();
    m_detailViewsChart = new LineChart;
    m_detailViewsChart->setAccessibleName(QStringLiteral("Channel views chart"));
    m_detailViewsChart->setMaximumHeight(210);
    m_detailViewsChart->setMinimumWidth(240);
    m_detailViewsPanel->body()->addWidget(m_detailViewsChart);
    m_detailDiscordChartPanel = new DashboardPanel(QStringLiteral("Posts over time"));
    m_detailDiscordChartPanel->setObjectName(QStringLiteral("marketingDiscordPostsChartPanel"));
    m_detailDiscordChartPanel->setCompact();
    m_detailDiscordChart = new LineChart;
    m_detailDiscordChart->setAccessibleName(QStringLiteral("Discord posts per day chart"));
    m_detailDiscordChart->setMaximumHeight(210);
    m_detailDiscordChart->setMinimumWidth(240);
    m_detailDiscordChartPanel->body()->addWidget(m_detailDiscordChart);
    m_detailDiscordChartPanel->hide();
    auto *detailCharts = clearWidget();
    auto *detailChartsLayout = new QHBoxLayout(detailCharts);
    detailChartsLayout->setContentsMargins(0, 0, 0, 0);
    detailChartsLayout->setSpacing(10);
    detailChartsLayout->addWidget(m_detailChartPanel, 1);
    detailChartsLayout->addWidget(m_detailViewsPanel, 1);
    detailChartsLayout->addWidget(m_detailDiscordChartPanel, 1);
    detailGrid->addPanel(detailCharts, 2);
    auto *detailSide = clearWidget();
    auto *detailSideLayout = new QVBoxLayout(detailSide);
    detailSideLayout->setContentsMargins(0, 0, 0, 0);
    detailSideLayout->setSpacing(10);
    detailSideLayout->addWidget(m_detailHead);
    auto *stepsPanel = new DashboardPanel(QStringLiteral("Next steps"));
    stepsPanel->setCompact();
    m_detailSteps = new QVBoxLayout;
    m_detailSteps->setSpacing(4);
    stepsPanel->body()->addLayout(m_detailSteps);
    detailSideLayout->addWidget(stepsPanel);
    detailSideLayout->addStretch(1);
    detailGrid->addPanel(detailSide, 1);
    m_detailDeskPanel = new DashboardPanel(QStringLiteral("Desk"));
    m_detailDeskPanel->setCompact();
    auto *tagLink = linkButton(QStringLiteral("Tag a link"),
        QStringLiteral("Open the link builder with this channel as the source"));
    m_detailDeskPanel->tools()->addWidget(tagLink);
    connect(tagLink, &QPushButton::clicked, this, [this] { tagChannelLink(m_detailChannel); });
    m_detailDesk = new TileGrid(168, 6);
    m_detailDesk->setAccessibleName(QStringLiteral("Channel desk"));
    m_detailDeskPanel->body()->addWidget(m_detailDesk);
    detailGrid->addPanel(m_detailDeskPanel, 3);
    m_detailRedditBox = clearWidget();
    m_detailRedditBox->setAccessibleName(QStringLiteral("Reddit desk"));
    auto *redditBoxLayout = new QVBoxLayout(m_detailRedditBox);
    redditBoxLayout->setContentsMargins(0, 0, 0, 0);
    redditBoxLayout->setSpacing(10);
    detailGrid->addPanel(m_detailRedditBox, 3);
    m_detailRedditBox->hide();
    m_detailHealth = detailBox(QStringLiteral("Health and settings"), 1);
    m_detailPlaybook = detailBox(QStringLiteral("Playbook"), 1);

    m_detailAdsPanel = new DashboardPanel(QStringLiteral("Ads"));
    m_detailAdsPanel->setCompact();
    m_detailAdsPanel->body()->setContentsMargins(6, 4, 6, 8);
    m_detailAds = new QTableWidget(0, 3);
    m_detailAds->setObjectName(QStringLiteral("marketingChannelAdsTable"));
    m_detailAds->setHorizontalHeaderLabels(
        {QStringLiteral("Ad"), QStringLiteral("Status"), QStringLiteral("Spend")});
    dashboardTable(m_detailAds);
    m_detailAds->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_detailAds->setSelectionMode(QAbstractItemView::SingleSelection);
    m_detailAds->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_detailAds->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_detailAdsPanel->body()->addWidget(m_detailAds);
    detailGrid->addPanel(m_detailAdsPanel, 1);

    m_detailEmails = detailBox(QStringLiteral("Suggested emails"), 3, &m_detailEmailsPanel);
    m_detailEmailsPanel->setObjectName(QStringLiteral("marketingEmailSuggestionsPanel"));
    auto *checkEmails = linkButton(QStringLiteral("Check again"),
        QStringLiteral("Look through CRM again for people to email"));
    m_detailEmailsPanel->tools()->addWidget(checkEmails);
    connect(checkEmails, &QPushButton::clicked, this, [this] {
        if (m_emailSuggestionsLoading)
            return;
        m_emailSuggestionsOpened.clear();
        loadEmailSuggestions();
    });
    m_detailEmailsPanel->hide();

    m_detailDiscordPanel = new DashboardPanel(QStringLiteral("Discord posts in the last 6 days"));
    m_detailDiscordPanel->setObjectName(QStringLiteral("marketingDiscordPostsPanel"));
    m_detailDiscordPanel->setCompact();
    m_detailDiscordPanel->body()->setContentsMargins(6, 4, 6, 8);
    auto *checkDiscord = linkButton(QStringLiteral("Check again"),
        QStringLiteral("Read the Discord channels again"));
    m_detailDiscordPanel->tools()->addWidget(checkDiscord);
    connect(checkDiscord, &QPushButton::clicked, this, [this] { loadDiscordPosts(true); });
    m_detailDiscordNote = new QLabel;
    m_detailDiscordNote->setObjectName(QStringLiteral("marketingHint"));
    m_detailDiscordNote->setWordWrap(true);
    m_detailDiscordNote->setContentsMargins(2, 2, 2, 0);
    m_detailDiscordPanel->body()->addWidget(m_detailDiscordNote);
    m_detailDiscord = new QTableWidget(0, 4);
    m_detailDiscord->setObjectName(QStringLiteral("marketingDiscordPostsTable"));
    m_detailDiscord->setHorizontalHeaderLabels(
        {QStringLiteral("Posted"), QStringLiteral("Channel"), QStringLiteral("Author"), QStringLiteral("Message")});
    dashboardTable(m_detailDiscord);
    m_detailDiscord->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_detailDiscord->setSelectionMode(QAbstractItemView::SingleSelection);
    m_detailDiscord->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_detailDiscord->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_detailDiscordPanel->body()->addWidget(m_detailDiscord);
    connect(m_detailDiscord, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {
        const QUrl url = QUrl::fromEncoded(
            m_detailDiscord->item(item->row(), 0)->data(Qt::UserRole).toString().toUtf8(), QUrl::StrictMode);
        if (url.isValid() && url.scheme() == QLatin1String("https") &&
            url.host() == QLatin1String("discord.com"))
            QDesktopServices::openUrl(url);
    });
    detailGrid->addPanel(m_detailDiscordPanel, 3);
    m_detailDiscordPanel->hide();

    m_detailReady = detailBox(QStringLiteral("Ready to post"), 3, &m_detailReadyPanel);
    m_detailReadyPanel->setObjectName(QStringLiteral("marketingReadyPostPanel"));
    auto *checkReady = linkButton(QStringLiteral("Check again"),
        QStringLiteral("Load the ready X post again"));
    m_detailReadyPanel->tools()->addWidget(checkReady);
    connect(checkReady, &QPushButton::clicked, this, [this] { loadReadyPost(); });
    m_detailReadyPanel->hide();
    m_readyPostTimer = new QTimer(this);
    m_readyPostTimer->setSingleShot(true);
    m_readyPostTimer->setInterval(45000);
    connect(m_readyPostTimer, &QTimer::timeout, this, [this] {
        if (m_detailChannel == QLatin1String("x"))
            loadReadyPost();
    });

    auto *detailWork = clearWidget();
    auto *detailWorkLayout = new QHBoxLayout(detailWork);
    detailWorkLayout->setContentsMargins(0, 0, 0, 0);
    detailWorkLayout->setSpacing(10);
    m_detailPeople = sideBox(detailWorkLayout, QStringLiteral("People to follow"), &m_detailPeoplePanel);
    m_detailPeoplePanel->setMinimumWidth(286);
    m_detailPeoplePanel->setMaximumWidth(340);
    auto *findPeople = linkButton(QStringLiteral("Find more"),
        QStringLiteral("Look again for active people posting about your topics"));
    m_detailPeoplePanel->tools()->addWidget(findPeople);
    connect(findPeople, &QPushButton::clicked, this, [this] { loadMastodon(); });
    m_detailPeoplePanel->hide();

    m_detailContentPanel = new DashboardPanel(QStringLiteral("Posts and content"));
    m_detailContentPanel->setCompact();
    m_detailContentPanel->body()->setContentsMargins(6, 4, 6, 8);
    auto *addPostLink = linkButton(QStringLiteral("Add link"), QStringLiteral("Track the stats of something you posted on this channel"));
    m_detailContentPanel->tools()->addWidget(addPostLink);
    auto *newPostLink = linkButton(QStringLiteral("New post"), QStringLiteral("Draft a post for this channel"));
    m_detailContentPanel->tools()->addWidget(newPostLink);
    m_detailPostsNote = new QLabel;
    m_detailPostsNote->setObjectName(QStringLiteral("marketingHint"));
    m_detailPostsNote->setWordWrap(true);
    m_detailPostsNote->setContentsMargins(2, 2, 2, 0);
    m_detailContentPanel->body()->addWidget(m_detailPostsNote);
    m_detailPosts = new QTableWidget(0, 9);
    m_detailPosts->setObjectName(QStringLiteral("marketingChannelPostsTable"));
    m_detailPosts->setHorizontalHeaderLabels(
        {QStringLiteral("Post"), QStringLiteral("Channel"), QStringLiteral("Posted"), QStringLiteral("Views"),
         QStringLiteral("Likes"), QStringLiteral("Replies"), QStringLiteral("Reposts"), QStringLiteral("Checked"),
         QStringLiteral("Added by")});
    dashboardTable(m_detailPosts);
    m_detailPosts->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_detailPosts->setSelectionMode(QAbstractItemView::SingleSelection);
    m_detailPosts->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_detailPosts->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_detailContentPanel->body()->addWidget(m_detailPosts);
    m_detailContent = new QTableWidget(0, 6);
    m_detailContent->setObjectName(QStringLiteral("marketingChannelContentTable"));
    m_detailContent->setHorizontalHeaderLabels(
        {QStringLiteral("Title"), QStringLiteral("Status"), QStringLiteral("When"), QStringLiteral("Views"),
         QStringLiteral("Visitors"), QStringLiteral("Campaign")});
    dashboardTable(m_detailContent);
    m_detailContent->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_detailContent->setSelectionMode(QAbstractItemView::SingleSelection);
    m_detailContent->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_detailContent->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_detailContentPanel->body()->addWidget(m_detailContent);
    detailWorkLayout->addWidget(m_detailContentPanel, 1);
    detailGrid->addPanel(detailWork, 2);

    auto *detailFind = clearWidget();
    auto *detailFindLayout = new QVBoxLayout(detailFind);
    detailFindLayout->setContentsMargins(0, 0, 0, 0);
    detailFindLayout->setSpacing(10);
    m_detailSearches = sideBox(detailFindLayout, QStringLiteral("Searches to check"), &m_detailSearchesPanel);
    m_detailSearchesPanel->hide();

    m_detailSnapshotsPanel = new DashboardPanel(QStringLiteral("Recorded numbers"));
    m_detailSnapshotsPanel->setCompact();
    m_detailSnapshotsPanel->body()->setContentsMargins(6, 4, 6, 8);
    m_detailSnapshots = new QTableWidget(0, 12);
    m_detailSnapshots->setObjectName(QStringLiteral("marketingChannelSnapshotsTable"));
    m_detailSnapshots->setHorizontalHeaderLabels(
        {QStringLiteral("Recorded"), QStringLiteral("Followers"), QStringLiteral("Following"), QStringLiteral("Posts"),
         QStringLiteral("Reach"), QStringLiteral("Engagements"), QStringLiteral("Views"), QStringLiteral("Visits"),
         QStringLiteral("Stars"), QStringLiteral("Forks"),
         QStringLiteral("Source"), QStringLiteral("Note")});
    dashboardTable(m_detailSnapshots);
    m_detailSnapshots->setSelectionMode(QAbstractItemView::NoSelection);
    m_detailSnapshots->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_detailSnapshots->horizontalHeader()->setSectionResizeMode(11, QHeaderView::Stretch);
    m_detailSnapshotsPanel->body()->addWidget(m_detailSnapshots);
    detailFindLayout->addWidget(m_detailSnapshotsPanel);
    detailFindLayout->addStretch(1);
    detailGrid->addPanel(detailFind, 1);

    m_attentionPanel = new DashboardPanel(QStringLiteral("Needs attention"));
    m_attentionPanel->setObjectName(QStringLiteral("marketingAttentionPanel"));
    m_attentionPanel->setCompact();
    m_attentionMore = linkButton(QString(), QStringLiteral("Show or hide the rest of the list"));
    m_attentionPanel->tools()->addWidget(m_attentionMore);
    m_attentionGrid = new TileGrid(300, 4);
    m_attentionGrid->setAccessibleName(QStringLiteral("What needs attention"));
    m_attentionPanel->body()->addWidget(m_attentionGrid);
    grid->addPanel(m_attentionPanel, 3);

    m_trafficPanel = new DashboardPanel(QStringLiteral("Web traffic by channel"));
    m_trafficPanel->setCompact();
    m_trafficPanel->body()->setContentsMargins(6, 4, 6, 8);
    m_trafficSetup = new QLabel;
    m_trafficSetup->setObjectName(QStringLiteral("marketingNote"));
    m_trafficSetup->setWordWrap(true);
    m_trafficSetup->setContentsMargins(8, 8, 8, 8);
    m_trafficPanel->body()->addWidget(m_trafficSetup);
    m_trafficTable = new QTableWidget(0, 8);
    m_trafficTable->setObjectName(QStringLiteral("marketingTrafficTable"));
    m_trafficTable->setHorizontalHeaderLabels(
        {QStringLiteral("Channel"), QStringLiteral("Share of visits"), QStringLiteral("Visits"),
         QStringLiteral("Change"), QStringLiteral("Conv. rate"), QStringLiteral("Avg time"),
         QStringLiteral("Bounce"), QStringLiteral("Pages / visit")});
    dashboardTable(m_trafficTable);
    m_trafficTable->verticalHeader()->setDefaultSectionSize(26);
    m_trafficTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_trafficTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_trafficTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_trafficPanel->body()->addWidget(m_trafficTable);
    grid->addPanel(m_trafficPanel, 2);

    auto *keywordsPanel = new DashboardPanel(QStringLiteral("Search keywords"));
    keywordsPanel->setCompact();
    keywordsPanel->body()->setContentsMargins(6, 4, 6, 8);
    m_keywordView = new QButtonGroup(this);
    keywordsPanel->tools()->addWidget(segmented(
        {QStringLiteral("Tracked"), QStringLiteral("From visits")}, m_keywordView, 0,
        QStringLiteral("Keyword source")));
    auto *addKeyword = linkButton(QStringLiteral("Add"), QStringLiteral("Track a search keyword"));
    keywordsPanel->tools()->addWidget(addKeyword);
    auto *importKeywordsLink = linkButton(QStringLiteral("Import"),
        QStringLiteral("Paste a Search Console or Bing keyword export"));
    keywordsPanel->tools()->addWidget(importKeywordsLink);
    m_keywordsTable = new QTableWidget(0, 5);
    m_keywordsTable->setObjectName(QStringLiteral("marketingKeywordsTable"));
    dashboardTable(m_keywordsTable);
    m_keywordsTable->verticalHeader()->setDefaultSectionSize(26);
    m_keywordsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_keywordsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    keywordsPanel->body()->addWidget(m_keywordsTable);
    m_keywordsEmpty = new QLabel;
    m_keywordsEmpty->setObjectName(QStringLiteral("marketingHint"));
    m_keywordsEmpty->setWordWrap(true);
    m_keywordsEmpty->setAlignment(Qt::AlignCenter);
    m_keywordsEmpty->setContentsMargins(12, 12, 12, 12);
    keywordsPanel->body()->addWidget(m_keywordsEmpty);
    grid->addPanel(keywordsPanel, 1);

    m_referrersPanel = new DashboardPanel(QStringLiteral("Referring websites"));
    m_referrersPanel->setCompact();
    m_referrers = new BarRows;
    m_referrers->setAccessibleName(QStringLiteral("Referring websites"));
    m_referrers->onClicked = [this](int index) {
        const QJsonArray board = m_referringSites.value(QStringLiteral("board")).toArray();
        if (index < 0 || index >= board.size())
            return;
        const QUrl url(text(board.at(index).toObject(), "url"));
        if (url.isValid() && (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http")))
            QDesktopServices::openUrl(url);
    };
    m_referrersPanel->body()->addWidget(m_referrers);
    m_referrersNote = new QLabel;
    m_referrersNote->setObjectName(QStringLiteral("marketingHint"));
    m_referrersNote->setWordWrap(true);
    m_referrersPanel->body()->addWidget(m_referrersNote);
    grid->addPanel(m_referrersPanel, 1);

    auto *campaignsPanel = new DashboardPanel(QStringLiteral("Campaigns"));
    campaignsPanel->setCompact();
    m_campaignTab = new QButtonGroup(this);
    campaignsPanel->tools()->addWidget(segmented(
        {QStringLiteral("Open"), QStringLiteral("Done"), QStringLiteral("All")}, m_campaignTab, 0,
        QStringLiteral("Campaign status")));
    auto *newCampaignButton = new QPushButton(QStringLiteral("New campaign"));
    newCampaignButton->setObjectName(QStringLiteral("primaryButton"));
    campaignsPanel->tools()->addWidget(newCampaignButton);
    m_campaignGrid = new TileGrid(300, 8);
    campaignsPanel->body()->addWidget(m_campaignGrid);
    grid->addPanel(campaignsPanel, 2);

    m_agentPanel = new DashboardPanel(QStringLiteral("Marketing agent"));
    m_agentPanel->setCompact();
    m_assistantForm = clearWidget();
    auto *askLayout = new QGridLayout(m_assistantForm);
    askLayout->setContentsMargins(0, 0, 0, 0);
    askLayout->setSpacing(6);
    m_assistantCampaign = new QComboBox;
    m_assistantCampaign->setAccessibleName(QStringLiteral("Campaign to plan"));
    m_assistantQuestion = lineEdit();
    m_assistantQuestion->setMaxLength(600);
    m_assistantQuestion->setPlaceholderText(QStringLiteral("Ask, e.g. what should we post this week?"));
    m_assistantQuestion->setAccessibleName(QStringLiteral("Question for the Marketing agent"));
    m_assistantRun = new QPushButton(QStringLiteral("Suggest"));
    m_assistantRun->setObjectName(QStringLiteral("primaryButton"));
    askLayout->addWidget(m_assistantCampaign, 0, 0, 1, 2);
    askLayout->addWidget(m_assistantQuestion, 1, 0);
    askLayout->addWidget(m_assistantRun, 1, 1);
    m_agentPanel->body()->addWidget(m_assistantForm);
    m_assistantBox = new QVBoxLayout;
    m_assistantBox->setSpacing(6);
    m_agentPanel->body()->addLayout(m_assistantBox);
    grid->addPanel(m_agentPanel, 1);

    auto *flowPanel = new DashboardPanel(QStringLiteral("Content flow"));
    flowPanel->setCompact();
    auto *logMetricsLink = linkButton(QStringLiteral("Log metrics"));
    auto *newContentLink = linkButton(QStringLiteral("New"), QStringLiteral("New content"));
    flowPanel->tools()->addWidget(logMetricsLink);
    flowPanel->tools()->addWidget(newContentLink);
    m_stages = new BarRows;
    m_stages->setAccessibleName(QStringLiteral("Content by status"));
    flowPanel->body()->addWidget(m_stages);
    m_flowNotes = new QLabel;
    m_flowNotes->setObjectName(QStringLiteral("marketingNote"));
    m_flowNotes->setWordWrap(true);
    m_flowNotes->setTextFormat(Qt::RichText);
    flowPanel->body()->addWidget(m_flowNotes);
    grid->addPanel(flowPanel, 1);

    auto *adsOverviewPanel = new DashboardPanel(QStringLiteral("Ads"));
    adsOverviewPanel->setCompact();
    auto *adsManagerLink = linkButton(QStringLiteral("Ads manager"));
    auto *newAdLink = linkButton(QStringLiteral("New ad"));
    adsOverviewPanel->tools()->addWidget(adsManagerLink);
    adsOverviewPanel->tools()->addWidget(newAdLink);
    m_adsOverviewBox = new QVBoxLayout;
    m_adsOverviewBox->setSpacing(6);
    adsOverviewPanel->body()->addLayout(m_adsOverviewBox);
    grid->addPanel(adsOverviewPanel, 1);

    m_xPanel = new DashboardPanel(QStringLiteral("Post to X"));
    m_xPanel->setCompact();
    m_xConnect = linkButton(QStringLiteral("Connect"));
    m_xPanel->tools()->addWidget(m_xConnect);
    m_xStatus = new QLabel;
    m_xStatus->setObjectName(QStringLiteral("marketingHint"));
    m_xStatus->setWordWrap(true);
    m_xStatus->setTextFormat(Qt::RichText);
    m_xStatus->setOpenExternalLinks(true);
    m_xPanel->body()->addWidget(m_xStatus);
    m_xContent = new QComboBox;
    m_xContent->setAccessibleName(QStringLiteral("Pipeline item to post"));
    m_xPanel->body()->addWidget(m_xContent);
    m_xText = new QPlainTextEdit;
    m_xText->setPlaceholderText(QStringLiteral("What's happening?"));
    m_xText->setAccessibleName(QStringLiteral("Post text"));
    m_xText->setFixedHeight(72);
    m_xPanel->body()->addWidget(m_xText);
    auto *xFoot = new QHBoxLayout;
    m_xCount = new QLabel;
    m_xCount->setObjectName(QStringLiteral("marketingHint"));
    xFoot->addWidget(m_xCount);
    xFoot->addStretch();
    m_xPost = new QPushButton(QStringLiteral("Post to X"));
    m_xPost->setObjectName(QStringLiteral("primaryButton"));
    xFoot->addWidget(m_xPost);
    m_xPanel->body()->addLayout(xFoot);
    grid->addPanel(m_xPanel, 1);

    m_contentPanel = new DashboardPanel(QStringLiteral("Content pipeline"));
    m_contentPanel->setCompact();
    auto *contentFilters = new QHBoxLayout;
    contentFilters->setSpacing(6);
    m_contentSearch = lineEdit();
    m_contentSearch->setPlaceholderText(QStringLiteral("Search content"));
    m_contentSearch->setAccessibleName(QStringLiteral("Search content"));
    contentFilters->addWidget(m_contentSearch, 1);
    m_contentStatus = new QComboBox;
    m_contentStatus->setAccessibleName(QStringLiteral("Status"));
    m_contentStatus->addItem(QStringLiteral("All statuses"), QString());
    for (const QString &status : contentStatuses())
        m_contentStatus->addItem(channelName(status), status);
    contentFilters->addWidget(m_contentStatus);
    m_contentChannel = new QComboBox;
    m_contentChannel->setAccessibleName(QStringLiteral("Channel"));
    m_contentChannel->addItem(QStringLiteral("All channels"), QString());
    contentFilters->addWidget(m_contentChannel);
    m_contentPanel->body()->addLayout(contentFilters);
    m_contentTable = new QTableWidget(0, 7);
    m_contentTable->setObjectName(QStringLiteral("marketingContentTable"));
    m_contentTable->setHorizontalHeaderLabels(
        {QStringLiteral("Title"), QStringLiteral("Channel"),
         QStringLiteral("Status"), QStringLiteral("Campaign"),
         QStringLiteral("Owner"), QStringLiteral("Scheduled"),
         QStringLiteral("Updated")});
    dashboardTable(m_contentTable);
    m_contentTable->verticalHeader()->setDefaultSectionSize(26);
    m_contentTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_contentTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_contentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_contentTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_contentPanel->body()->addWidget(m_contentTable);
    auto *contentActions = new QHBoxLayout;
    auto *composeButton = new QPushButton(QStringLiteral("Post to X"));
    composeButton->setToolTip(QStringLiteral("Write the selected item into the Post to X box"));
    auto *editButton = new QPushButton(QStringLiteral("Edit"));
    auto *deleteButton = new QPushButton(QStringLiteral("Delete"));
    deleteButton->setObjectName(QStringLiteral("dangerButton"));
    contentActions->addStretch();
    contentActions->addWidget(composeButton);
    contentActions->addWidget(editButton);
    contentActions->addWidget(deleteButton);
    m_contentPanel->body()->addLayout(contentActions);
    grid->addPanel(m_contentPanel, 2);

    m_weekPanel = new DashboardPanel(QStringLiteral("This week"));
    m_weekPanel->setCompact();
    m_weekBox = new QVBoxLayout;
    m_weekBox->setSpacing(0);
    m_weekPanel->body()->addLayout(m_weekBox);
    grid->addPanel(m_weekPanel, 1);

    m_chartPanel = new DashboardPanel(QStringLiteral("Reach by channel"));
    m_chartPanel->setCompact();
    m_chartMetric = new QButtonGroup(this);
    m_chartPanel->tools()->addWidget(segmented(
        {QStringLiteral("Reach"), QStringLiteral("Engagements"), QStringLiteral("Followers")},
        m_chartMetric, 0, QStringLiteral("Chart metric")));
    m_chartLegend = new QLabel;
    m_chartLegend->setObjectName(QStringLiteral("marketingHint"));
    m_chartLegend->setTextFormat(Qt::RichText);
    m_chartLegend->setWordWrap(true);
    m_chartPanel->body()->addWidget(m_chartLegend);
    m_chart = new LineChart;
    m_chart->setAccessibleName(QStringLiteral("Channel metric chart"));
    m_chart->setMaximumHeight(210);
    m_chartPanel->body()->addWidget(m_chart);
    grid->addPanel(m_chartPanel, 2);

    auto *landingPanel = new DashboardPanel(QStringLiteral("Top landing pages"));
    landingPanel->setCompact();
    m_landing = new BarRows;
    m_landing->setAccessibleName(QStringLiteral("Top landing pages"));
    landingPanel->body()->addWidget(m_landing);
    grid->addPanel(landingPanel, 1);
    grid->setMovable(QStringLiteral("market/packages/marketing/data/overviewPanels"));

    m_adsPanel = new DashboardPanel(QStringLiteral("Ads manager"));
    m_adsSummary = new QLabel;
    m_adsSummary->setObjectName(QStringLiteral("marketingHint"));
    m_adsSummary->setWordWrap(true);
    m_adsPanel->body()->addWidget(m_adsSummary);
    m_adsTable = new QTableWidget(0, 13);
    m_adsTable->setObjectName(QStringLiteral("marketingAdsTable"));
    m_adsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Ad"), QStringLiteral("Channel"), QStringLiteral("Bought in"),
         QStringLiteral("Status"), QStringLiteral("Flight"), QStringLiteral("Budget"),
         QStringLiteral("Spend"), QStringLiteral("Pacing"),
         QStringLiteral("Impressions"), QStringLiteral("CTR"),
         QStringLiteral("Conversions"), QStringLiteral("CPA"), QStringLiteral("ROAS")});
    dashboardTable(m_adsTable);
    m_adsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_adsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_adsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_adsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_adsPanel->body()->addWidget(m_adsTable);
    auto *adActions = new QHBoxLayout;
    auto *adsNote = new QLabel(QStringLiteral(
        "Plan and measure ads for any channel here; buy them in each platform's console. Results are logged by hand and nothing here spends money."));
    adsNote->setObjectName(QStringLiteral("marketingHint"));
    adsNote->setWordWrap(true);
    adActions->addWidget(adsNote, 1);
    auto *newAdButton = new QPushButton(QStringLiteral("New ad"));
    newAdButton->setObjectName(QStringLiteral("primaryButton"));
    auto *editAdButton = new QPushButton(QStringLiteral("Edit"));
    auto *resultsButton = new QPushButton(QStringLiteral("Log results"));
    auto *consoleButton = new QPushButton(QStringLiteral("Open console"));
    consoleButton->setToolTip(QStringLiteral(
        "Open the selected ad's own link, or its channel's ad console, in the browser"));
    auto *deleteAdButton = new QPushButton(QStringLiteral("Delete"));
    deleteAdButton->setObjectName(QStringLiteral("dangerButton"));
    for (QPushButton *button : {newAdButton, editAdButton, resultsButton,
                                consoleButton, deleteAdButton})
        adActions->addWidget(button);
    m_adsPanel->body()->addLayout(adActions);

    m_channelsPanel = new DashboardPanel(QStringLiteral("Manage channels"));
    m_saveChannelsButton = new QPushButton(QStringLiteral("Save channels"));
    m_saveChannelsButton->setObjectName(QStringLiteral("ghostButton"));
    m_channelsPanel->tools()->addWidget(m_saveChannelsButton);
    auto *velocityRow = new QHBoxLayout;
    auto *velocityLabel = new QLabel(QStringLiteral("Posting velocity"));
    velocityLabel->setObjectName(QStringLiteral("marketingHint"));
    velocityRow->addWidget(velocityLabel);
    m_velocityWindow = new QButtonGroup(this);
    velocityRow->addWidget(segmented({QStringLiteral("30 days"), QStringLiteral("7 days"),
                                      QStringLiteral("24 hours"), QStringLiteral("30 minutes")},
                                     m_velocityWindow, 0, QStringLiteral("Posting velocity over")));
    auto *velocityNote = new QLabel(QStringLiteral(
        "Bars count published posts by the time they went out. Refresh a channel to import its latest posts and stats."));
    velocityNote->setObjectName(QStringLiteral("marketingHint"));
    velocityNote->setWordWrap(true);
    velocityRow->addWidget(velocityNote, 1);
    m_channelsPanel->body()->addLayout(velocityRow);
    m_channelsTable = new QTableWidget(0, 18);
    m_channelsTable->setObjectName(QStringLiteral("marketingChannelsTable"));
    m_channelsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Channel"), QStringLiteral("Active"),
         QStringLiteral("Display name"), QStringLiteral("Channel URL"),
         QStringLiteral("Velocity"), QStringLiteral("Reach"),
         QStringLiteral("Followers"), QStringLiteral("Impressions"),
         QStringLiteral("Engagements"), QStringLiteral("Clicks"),
         QStringLiteral("Views"), QStringLiteral("Visits · 30d"),
         QStringLiteral("Pages on Google"), QStringLiteral("Emails sent · 30d"),
         QStringLiteral("Closed deals"), QStringLiteral("Stars"),
         QStringLiteral("Forks"), QStringLiteral("Status")});
    dashboardTable(m_channelsTable);
    m_channelsTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_channelsTable->verticalHeader()->setDefaultSectionSize(38);
    m_channelsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    for (int column : {0, 1, 2, 3, 4})
        m_channelsTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Interactive);
    m_channelsTable->horizontalHeader()->setSectionResizeMode(17, QHeaderView::Stretch);
    m_channelsTable->setColumnWidth(0, 210);
    m_channelsTable->setColumnWidth(1, 56);
    m_channelsTable->setColumnWidth(2, 150);
    m_channelsTable->setColumnWidth(3, 280);
    m_channelsTable->setColumnWidth(4, 250);
    m_channelsTable->setAlternatingRowColors(true);
    m_channelsPanel->body()->addWidget(m_channelsTable);

    m_catalogPanel = new DashboardPanel(QStringLiteral("Add channels"));
    auto *catalogNote = new QLabel(QStringLiteral(
        "Choose from 100 channels used by marketing teams worldwide. Add any channel to the dashboard with one click. Availability is not a compliance certification; follow platform terms and the marketing rules of every market you publish in."));
    catalogNote->setObjectName(QStringLiteral("marketingHint"));
    catalogNote->setWordWrap(true);
    m_catalogPanel->body()->addWidget(catalogNote);
    m_catalogTable = new QTableWidget(0, 4);
    m_catalogTable->setObjectName(QStringLiteral("marketingChannelCatalogTable"));
    m_catalogTable->setHorizontalHeaderLabels(
        {QStringLiteral("Rank"), QStringLiteral("Channel"),
         QStringLiteral("Category"), QStringLiteral("Dashboard")});
    dashboardTable(m_catalogTable);
    m_catalogTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_catalogTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_catalogTable->setAlternatingRowColors(true);
    m_catalogPanel->body()->addWidget(m_catalogTable);
    buildPages(outer);

    connect(m_organization, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || m_loading)
            return;
        m_traffic = QJsonObject();
        m_trafficLoaded = false;
        m_assistant = QJsonObject();
        m_performance = QJsonObject();
        m_performanceProject.clear();
        closeChannel();
        loadOrganization();
    });
    connect(refreshButton, &QPushButton::clicked, this,
            [this] {
                if (m_installed)
                    refreshAllChannels();
                else
                    refreshOrganizations();
            });
    connect(m_installButton, &QPushButton::clicked, this,
            [this] { installPlugin(); });
    connect(m_newButton, &QPushButton::clicked, this,
            [this] { editContent(); });
    connect(m_metricButton, &QPushButton::clicked, this,
            [this] { addMetric(); });
    connect(m_webButton, &QPushButton::clicked, this, [this] { openWebDashboard(); });
    connect(m_rangeGroup, &QButtonGroup::idClicked, this, [this](int id) {
        static constexpr int ranges[] = {7, 30, 90};
        m_rangeDays = ranges[qBound(0, id, 2)];
        renderKpis();
        renderChart();
        renderCampaigns();
        renderChannelDetail();
        loadTraffic();
    });
    connect(m_saveChannelsButton, &QPushButton::clicked, this,
            [this] {
                saveChannels([this] {
                    setStatus(QStringLiteral("Channel settings saved. Updating dashboard statistics…"));
                    loadOrganization();
                });
            });
    connect(m_velocityWindow, &QButtonGroup::idClicked, this,
            [this](int) { updateVelocityCharts(); });
    // The 24-hour and 30-minute windows move with the clock.
    m_velocityTimer = new QTimer(this);
    m_velocityTimer->setInterval(60 * 1000);
    connect(m_velocityTimer, &QTimer::timeout, this, [this] {
        if (isVisible())
            updateVelocityCharts();
    });
    m_velocityTimer->start();
    m_railRetry = new QTimer(this);
    m_railRetry->setSingleShot(true);
    connect(m_railRetry, &QTimer::timeout, this, [this] { refresh(); });
    m_changedReload = new QTimer(this);
    m_changedReload->setSingleShot(true);
    m_changedReload->setInterval(400);
    connect(m_changedReload, &QTimer::timeout, this, [this] {
        if (m_loading)
            return m_changedReload->start();
        if (m_installed && !m_organization->currentData().toString().isEmpty())
            loadOrganization();
        else
            refresh();
    });
    connect(addKeyword, &QPushButton::clicked, this, [this] { editKeyword(QJsonObject()); });
    connect(importKeywordsLink, &QPushButton::clicked, this, [this] { importKeywords(); });
    connect(m_keywordView, &QButtonGroup::idClicked, this, [this](int) { renderKeywords(); });
    connect(m_keywordsTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {
        QTableWidgetItem *cell = item ? m_keywordsTable->item(item->row(), 0) : nullptr;
        const QString id = cell ? cell->data(Qt::UserRole).toString() : QString();
        for (const QJsonValue &value : std::as_const(m_keywords)) {
            if (!id.isEmpty() && text(value.toObject(), "id") == id)
                return editKeyword(value.toObject());
        }
    });
    connect(m_attentionMore, &QPushButton::clicked, this, [this] {
        m_attentionAll = !m_attentionAll;
        renderAttention();
    });
    connect(m_campaignTab, &QButtonGroup::idClicked, this, [this](int) { renderCampaigns(); });
    connect(newCampaignButton, &QPushButton::clicked, this, [this] { editCampaign(QJsonObject()); });
    connect(m_assistantRun, &QPushButton::clicked, this, [this] { runAssistant(); });
    connect(m_assistantQuestion, &QLineEdit::returnPressed, this, [this] { runAssistant(); });
    m_stages->onClicked = [this](int index) {
        const int target = index + 1;
        m_contentStatus->setCurrentIndex(m_contentStatus->currentIndex() == target ? 0 : target);
        reveal(m_contentPanel);
    };
    connect(m_flowNotes, &QLabel::linkActivated, this, [this](const QString &link) {
        for (const QJsonValue &value : std::as_const(m_content)) {
            if (QStringLiteral("content:") + text(value.toObject(), "id") == link)
                return editContent(value.toObject());
        }
    });
    connect(logMetricsLink, &QPushButton::clicked, this, [this] { addMetric(); });
    connect(newContentLink, &QPushButton::clicked, this, [this] { editContent(); });
    connect(adsManagerLink, &QPushButton::clicked, this, [this] { reveal(m_adsPanel); });
    connect(newAdLink, &QPushButton::clicked, this, [this] { editAd(); });
    connect(m_trafficSetup, &QLabel::linkActivated, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://app.forkmesh.com/dashboard/analytics")));
    });
    connect(m_xConnect, &QPushButton::clicked, this, [this] { editXConnection(); });
    connect(m_xContent, &QComboBox::activated, this, [this](int index) {
        const QString id = m_xContent->itemData(index).toString();
        for (const QJsonValue &value : std::as_const(m_content)) {
            const QJsonObject item = value.toObject();
            if (!id.isEmpty() && text(item, "id") == id) {
                m_xText->setPlainText(text(item, "body").isEmpty() ? text(item, "title")
                                                                    : item.value(QStringLiteral("body")).toString());
                m_xContentId = id;
            }
        }
    });
    connect(m_xText, &QPlainTextEdit::textChanged, this, [this] { updateXCount(); });
    connect(m_xPost, &QPushButton::clicked, this, [this] { postToX(); });
    connect(m_contentSearch, &QLineEdit::textChanged, this, [this](const QString &) { renderContent(); });
    connect(m_contentStatus, &QComboBox::currentIndexChanged, this, [this](int) { renderContent(); });
    connect(m_contentChannel, &QComboBox::currentIndexChanged, this, [this](int) { renderContent(); });
    connect(m_chartMetric, &QButtonGroup::idClicked, this, [this](int) { renderChart(); });
    connect(composeButton, &QPushButton::clicked, this,
            [this] { composeFromContent(selectedContent()); });
    connect(editButton, &QPushButton::clicked, this,
            [this] { editContent(selectedContent()); });
    connect(deleteButton, &QPushButton::clicked, this,
            [this] { deleteContent(); });
    connect(m_contentTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *) { editContent(selectedContent()); });
    connect(newAdButton, &QPushButton::clicked, this, [this] { editAd(); });
    connect(editAdButton, &QPushButton::clicked, this, [this] {
        if (!selectedAd().isEmpty())
            editAd(selectedAd());
    });
    connect(resultsButton, &QPushButton::clicked, this, [this] { logAdResults(); });
    connect(consoleButton, &QPushButton::clicked, this, [this] { openAdConsole(); });
    connect(deleteAdButton, &QPushButton::clicked, this, [this] { deleteAd(); });
    connect(m_adsTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *) { editAd(selectedAd()); });
    connect(m_detailMetric, &QButtonGroup::idClicked, this, [this](int) { renderChannelChart(); });
    connect(newPostLink, &QPushButton::clicked, this, [this] {
        editContent(QJsonObject{{QStringLiteral("channel"), m_detailChannel},
                                {QStringLiteral("status"), QStringLiteral("draft")}});
    });
    connect(m_detailContent, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {
        const QString id = m_detailContent->item(item->row(), 0)->data(Qt::UserRole).toString();
        for (const QJsonValue &value : std::as_const(m_content)) {
            if (text(value.toObject(), "id") == id)
                return editContent(value.toObject());
        }
    });
    connect(addPostLink, &QPushButton::clicked, this, [this] { editPostedLink({}, m_detailChannel); });
    connect(m_detailPosts, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {
        const QString id = m_detailPosts->item(item->row(), 0)->data(Qt::UserRole).toString();
        for (const QJsonValue &value : std::as_const(m_posts)) {
            if (text(value.toObject(), "id") == id)
                return editPostedLink(value.toObject());
        }
    });
    connect(m_detailAds, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {
        const QString id = m_detailAds->item(item->row(), 0)->data(Qt::UserRole).toString();
        for (const QJsonValue &value : std::as_const(m_ads)) {
            if (text(value.toObject(), "id") == id)
                return editAd(value.toObject());
        }
    });
    m_detailScroll->hide();
    applyView();
    applyTheme();
    updateXCount();
}

void MarketingFeature::request(const QByteArray &method, const QString &path,
                               const QJsonObject &body, ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Marketing network access is unavailable."));
        return;
    }
    const QPointer<MarketingFeature> guard(this);
    m_request(method, path, body,
              [guard, handler = std::move(handler)](
                  bool ok, const QJsonObject &payload, const QString &error) mutable {
        if (guard)
            handler(ok, payload, error);
    });
}

QString MarketingFeature::organizationPath(const QString &suffix) const
{
    const QString name = m_organization->currentData().toString();
    return name.isEmpty() ? QStringLiteral("/api/marketing")
                          : QStringLiteral("/api/marketing/") + name + suffix;
}

QString MarketingFeature::pluginPath(const QString &suffix) const
{
    QString operation = suffix;
    if (operation.startsWith(QLatin1Char('/')))
        operation.remove(0, 1);
    return forkmesh::market::nativePackageLifecyclePath(
        m_organization->currentData().toString(), m_packageId, operation);
}

bool MarketingFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    const QString path = pluginPath(QStringLiteral("/export"));
    if (m_organization->currentData().toString().isEmpty() || path.isEmpty())
        return false;
    request(QByteArrayLiteral("GET"), path, {}, std::move(done));
    return true;
}

QJsonObject MarketingFeature::organizationRecord() const
{
    const QString name = m_organization->currentData().toString();
    for (const QJsonValue &value : m_organizations) {
        const QJsonObject item = value.toObject();
        if (text(item, "name") == name)
            return item;
    }
    return {};
}

void MarketingFeature::refresh()
{
    refreshOrganizations();
}

void MarketingFeature::packageChanged()
{
    m_changedReload->start();
}

void MarketingFeature::renderBrowserActivity()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList lines;
    for (const QJsonValue &value : std::as_const(m_browserActivityItems)) {
        const QJsonObject item = value.toObject();
        const qint64 at = qint64(item.value(QStringLiteral("at")).toDouble());
        if (at <= 0 || now - at > 24 * 60 * 60 * 1000LL)
            continue;
        const QString kind = text(item, "kind");
        const QString mark = kind == QLatin1String("failed") ? QStringLiteral("⚠")
                             : kind == QLatin1String("sent") ? QStringLiteral("↑")
                                                             : QStringLiteral("↻");
        QString line = QStringLiteral("%1 %2 %3")
            .arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(at).time(),
                                    QLocale::ShortFormat),
                 mark, text(item, "text").toHtmlEscaped());
        const QString url = text(item, "url");
        if (url.startsWith(QLatin1String("https://")))
            line += QStringLiteral(" <a href=\"%1\">Open</a>").arg(url.toHtmlEscaped());
        lines << line;
        if (lines.size() >= 5)
            break;
    }
    m_browserActivity->setVisible(!lines.isEmpty());
    m_browserActivity->setText(lines.isEmpty() ? QString()
        : QStringLiteral("<b>From your browser</b><br>") + lines.join(QStringLiteral("<br>")));
}

void MarketingFeature::refreshOrganizations()
{
    if (m_loading)
        return;
    m_loading = true;
    const QString previous = m_organization->currentData().toString();
    setStatus(QStringLiteral("Loading your organizations…"));
    request(QByteArrayLiteral("GET"), QStringLiteral("/api/marketing"), {},
            [this, previous](bool ok, const QJsonObject &payload,
                             const QString &error) {
        m_loading = false;
        if (!ok) {
            setStatus(QStringLiteral("Marketing could not load: %1").arg(error), true);
            railLoadFailed();
            return;
        }
        railLoaded();
        m_organizations = payload.value(QStringLiteral("organizations")).toArray();
        m_organization->blockSignals(true);
        m_organization->clear();
        for (const QJsonValue &value : m_organizations) {
            const QJsonObject item = value.toObject();
            QString label = text(item, "name") + QStringLiteral(" · ") + text(item, "role");
            if (!item.value(QStringLiteral("installed")).toBool())
                label += QStringLiteral(" · not installed");
            m_organization->addItem(label, text(item, "name"));
        }
        const int index = m_organization->findData(previous);
        m_organization->setCurrentIndex(index >= 0 ? index : 0);
        m_organization->blockSignals(false);
        if (m_organization->count()) {
            loadOrganization();
        } else {
            setStatus(QStringLiteral("Join or create an organization first."), true);
            m_railProblem = QStringLiteral("No organization yet");
            if (m_railChanged)
                m_railChanged();
        }
    });
}

void MarketingFeature::applyInstallState()
{
    const QJsonObject record = organizationRecord();
    const QString role = text(record, "role");
    const bool canManage = role == QLatin1String("owner") || role == QLatin1String("admin");
    m_installed = record.value(QStringLiteral("installed")).toBool();
    m_installPanel->setVisible(!m_installed);
    applyView();
    for (QPushButton *button : {m_newButton, m_metricButton, m_saveChannelsButton})
        button->setVisible(m_installed);
    m_saveChannelsButton->setEnabled(m_canManageSettings);
    m_installButton->setVisible(canManage);
    m_installNote->setText(canManage
        ? QStringLiteral("Install a shared content pipeline and channel analytics dashboard for this organization.")
        : QStringLiteral("Ask an organization owner or admin to install Marketing."));
    if (!m_installed && m_railChanged)
        m_railChanged();
}

void MarketingFeature::installPlugin()
{
    const QString path = pluginPath(QStringLiteral("/install"));
    if (path.isEmpty()) {
        setStatus(QStringLiteral(
            "Select a valid organization before installing Marketing."), true);
        return;
    }
    m_installButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), path, {},
            [this](bool ok, const QJsonObject &, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Marketing was not installed: %1").arg(error), true);
        setStatus(QStringLiteral("Marketing installed for this organization."));
        refreshOrganizations();
    });
}

void MarketingFeature::loadOrganization()
{
    if (m_loading || m_organization->currentData().toString().isEmpty())
        return;
    applyInstallState();
    if (!m_installed) {
        setStatus(QStringLiteral("Marketing is not installed for this organization."));
        return;
    }
    m_loading = true;
    request(QByteArrayLiteral("GET"), organizationPath(), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok) {
            railLoadFailed();
            return setStatus(QStringLiteral("Marketing could not load: %1").arg(error), true);
        }
        railLoaded();
        bool firstBlogRead = false;
        const QJsonObject settings = payload.value(QStringLiteral("settings")).toObject();
        for (const QJsonValue &value : settings.value(QStringLiteral("channels")).toArray()) {
            const QJsonObject channel = value.toObject();
            const QJsonObject feed = channel.value(QStringLiteral("feed")).toObject();
            if (text(channel, "id") == QLatin1String("blog") &&
                channel.value(QStringLiteral("enabled")).toBool() &&
                !text(channel, "profileUrl").isEmpty() &&
                feed.value(QStringLiteral("lastReadAt")).toDouble() <= 0 &&
                text(feed, "lastError").isEmpty()) {
                firstBlogRead = true;
                break;
            }
        }
        applyOrganization(payload);
        loadTraffic();
        if (m_page == 1 || m_detailChannel == QLatin1String("reddit"))
            loadPerformance();
        if (firstBlogRead)
            refreshChannel(QStringLiteral("blog"));
    });
}

void MarketingFeature::railLoadFailed()
{
    m_railProblem = QStringLiteral("Could not load");
    m_railRetryMs = qBound(30 * 1000, m_railRetryMs * 2, 15 * 60 * 1000);
    m_railRetry->start(m_railRetryMs);
    if (m_railChanged)
        m_railChanged();
}

void MarketingFeature::railLoaded()
{
    m_railProblem.clear();
    m_railRetryMs = 0;
    m_railRetry->stop();
}

bool MarketingFeature::refreshChannel(const QString &id)
{
    if (!m_installed || m_organization->currentData().toString().isEmpty() ||
        m_refreshingChannels.contains(id))
        return false;
    if (!m_channelEdits.isEmpty() && m_canManageSettings) {
        // Refresh reads the saved URL, so unsaved edits go first.
        saveChannels([this, id] { refreshChannel(id); });
        return true;
    }
    const QString name = channelName(id, m_settings, m_channelCatalog);
    m_refreshingChannels.insert(id);
    updateRefreshButtons();
    if (!m_refreshBatch)
        setStatus(QStringLiteral("Refreshing %1 stats and posts…").arg(name));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/refresh")),
            QJsonObject{{QStringLiteral("channel"), id}},
            [this, id, name](bool ok, const QJsonObject &payload, const QString &error) {
        m_refreshingChannels.remove(id);
        if (!ok) {
            const QString code = text(payload, "error");
            const QString message = QStringLiteral("%1 could not be refreshed: %2")
                .arg(name, refreshErrorText(id, code, error));
            updateRefreshButtons();
            if (m_refreshBatch) {
                m_refreshBatchErrors << message;
            } else {
                setStatus(message, true);
                if (code == QLatin1String("x_access_token_required") ||
                    code == QLatin1String("x_access_token_invalid") ||
                    code == QLatin1String("github_rate_limited"))
                    promptChannelToken(id);
            }
            advanceRefreshQueue();
            return;
        }
        applyOrganization(payload);
        const QJsonObject result = payload.value(QStringLiteral("refresh")).toObject();
        const int items = result.value(QStringLiteral("items")).toInt();
        QString message = QStringLiteral("%1 refreshed · %2 post%3 read · %4 new")
            .arg(name).arg(items).arg(items == 1 ? QString() : QStringLiteral("s"))
            .arg(result.value(QStringLiteral("imported")).toInt());
        const QJsonObject metrics = result.value(QStringLiteral("metrics")).toObject();
        if (id == QLatin1String("email")) {
            const qint64 deals = qint64(metrics.value(QStringLiteral("closedDeals")).toDouble());
            message = QStringLiteral("%1 refreshed · %2 emails sent in 30 days · %3 closed deal%4")
                .arg(name).arg(qint64(metrics.value(QStringLiteral("outbound")).toDouble()))
                .arg(deals).arg(deals == 1 ? QString() : QStringLiteral("s"));
            if (metrics.value(QStringLiteral("closedValue")).toDouble() > 0)
                message += QStringLiteral(" worth %1")
                    .arg(QLocale().toString(qint64(metrics.value(QStringLiteral("closedValue")).toDouble())));
            if (m_detailChannel == id)
                loadEmailSuggestions();
        }
        if (id == QLatin1String("discord") && m_detailChannel == id)
            loadDiscordPosts(true);
        if (id == QLatin1String("x") && m_detailChannel == id)
            loadReadyPost();
        if (id == QLatin1String("github"))
            message += QStringLiteral(" · %1 stars · %2 forks")
                .arg(qint64(metrics.value(QStringLiteral("stars")).toDouble()))
                .arg(qint64(metrics.value(QStringLiteral("forks")).toDouble()));
        if (metrics.contains(QStringLiteral("followers")) && id != QLatin1String("email"))
            message += QStringLiteral(" · %1 followers")
                .arg(qint64(metrics.value(QStringLiteral("followers")).toDouble()));
        if (id == QLatin1String("reddit") && metrics.isEmpty())
            message += QStringLiteral(" · Reddit gives no member counts here: press Update to read them with the browser extension");
        if (id == QLatin1String("google-search")) {
            message += QStringLiteral(" · %1 visits from Google in 30 days")
                .arg(qint64(metrics.value(QStringLiteral("visits")).toDouble()));
            if (metrics.value(QStringLiteral("indexedPages")).toDouble() > 0)
                message += QStringLiteral(" · %1 pages on Google")
                    .arg(qint64(metrics.value(QStringLiteral("indexedPages")).toDouble()));
        }
        if (!m_refreshBatch)
            setStatus(message + QLatin1Char('.'));
        advanceRefreshQueue();
    });
    return true;
}

void MarketingFeature::refreshAllChannels()
{
    if (m_refreshBatch)
        return;
    m_refreshQueue.clear();
    m_refreshBatchErrors.clear();
    for (const ChannelRow &row : std::as_const(m_channelRows)) {
        if (row.enabled && row.enabled->isChecked() &&
            (row.id == QLatin1String("email") ||
             (row.profileUrl && !row.profileUrl->text().trimmed().isEmpty())) &&
            m_refreshableChannels.contains(row.id))
            m_refreshQueue << row.id;
    }
    if (m_refreshQueue.isEmpty()) {
        loadOrganization();
        return;
    }
    m_refreshBatch = true;
    setStatus(QStringLiteral("Refreshing %1 channel%2…").arg(m_refreshQueue.size())
                  .arg(m_refreshQueue.size() == 1 ? QString() : QStringLiteral("s")));
    advanceRefreshQueue();
}

void MarketingFeature::advanceRefreshQueue()
{
    while (!m_refreshQueue.isEmpty()) {
        if (refreshChannel(m_refreshQueue.takeFirst()))
            return;
    }
    if (!m_refreshBatch)
        return;
    m_refreshBatch = false;
    if (m_refreshBatchErrors.isEmpty())
        setStatus(QStringLiteral("All active channels refreshed."));
    else
        setStatus(m_refreshBatchErrors.join(QStringLiteral(" · ")), true);
    m_refreshBatchErrors.clear();
}

void MarketingFeature::promptChannelToken(const QString &id)
{
    const QString name = channelName(id, m_settings, m_channelCatalog);
    if (!m_canManageSettings) {
        setStatus(QStringLiteral("%1 needs an API access token. Ask an organization owner or admin to add one.")
                      .arg(name), true);
        return;
    }
    const QPointer<MarketingFeature> guard(this);
    bool accepted = false;
    const QString token = QInputDialog::getText(
        this, QStringLiteral("%1 access token").arg(name),
        id == QLatin1String("x")
            ? QStringLiteral("X API bearer token with read access to the account's public posts:")
        : id == QLatin1String("mastodon")
            ? QStringLiteral("Mastodon access token with the read and write:follows scopes\n"
                             "(on your server: Preferences, Development, New application):")
            : QStringLiteral("GitHub personal access token (read-only public access raises the rate limit):"),
        QLineEdit::Password, QString(), &accepted).trimmed();
    if (!guard || !accepted || token.isEmpty())
        return;
    // Tokens are write-only: the Worker seals them and never returns them.
    QJsonObject edit = m_channelEdits.value(id);
    edit.insert(QStringLiteral("accessToken"), token);
    m_channelEdits.insert(id, edit);
    saveChannels([this, id] {
        if (id == QLatin1String("mastodon")) {
            m_mastodonOrg.clear();
            loadMastodon();
        }
        refreshChannel(id);
    });
}

void MarketingFeature::applyOrganization(const QJsonObject &payload)
{
    m_content = payload.value(QStringLiteral("content")).toArray();
    m_channelCatalog = payload.value(QStringLiteral("channelCatalog")).toArray();
    m_playbooks = payload.value(QStringLiteral("channelPlaybooks")).toObject();
    m_settings = payload.value(QStringLiteral("settings")).toObject();
    m_summaryData = payload.value(QStringLiteral("summary")).toObject();
    m_ads = payload.value(QStringLiteral("ads")).toArray();
    m_adsSummaryData = payload.value(QStringLiteral("adsSummary")).toObject();
    m_metrics = payload.value(QStringLiteral("metrics")).toArray();
    m_campaigns = payload.value(QStringLiteral("campaigns")).toArray();
    m_campaignTemplates = payload.value(QStringLiteral("campaignTemplates")).toArray();
    m_keywords = payload.value(QStringLiteral("keywords")).toArray();
    m_suggestions = payload.value(QStringLiteral("suggestions")).toArray();
    m_referringSites = payload.value(QStringLiteral("referringSites")).toObject();
    m_assistantAvailable = payload.value(QStringLiteral("assistantAvailable")).toBool();
    m_canManageSettings = payload.value(QStringLiteral("canManageSettings")).toBool();
    m_experiments = payload.value(QStringLiteral("experiments")).toArray();
    m_experimentsSummary = payload.value(QStringLiteral("experimentsSummary")).toObject();
    m_funnel = payload.value(QStringLiteral("funnel")).toArray();
    m_funnelReport = payload.value(QStringLiteral("funnelReport")).toObject();
    m_redditThreads = payload.value(QStringLiteral("redditThreads")).toArray();
    m_redditDesk = payload.value(QStringLiteral("redditDesk")).toObject();
    m_browserActivityItems = payload.value(QStringLiteral("browserActivity")).toArray();
    renderBrowserActivity();
    m_posts = payload.value(QStringLiteral("posts")).toArray();
    m_postsSummary = payload.value(QStringLiteral("postsSummary")).toObject();
    m_readyPostOverview = payload.value(QStringLiteral("readyPosts")).toObject()
                              .value(QStringLiteral("x")).toObject();
    m_refreshableChannels.clear();
    for (const QJsonValue &value :
         payload.value(QStringLiteral("refreshableChannels")).toArray())
        m_refreshableChannels << value.toString();
    if (m_refreshableChannels.isEmpty())
        m_refreshableChannels << QStringLiteral("blog");
    m_saveChannelsButton->setEnabled(m_canManageSettings);
    m_organization->setToolTip(QStringLiteral("%1 reach · %2 engagements · %3 published · %4 queued")
        .arg(qint64(m_summaryData.value(QStringLiteral("reach")).toDouble()))
        .arg(qint64(m_summaryData.value(QStringLiteral("engagements")).toDouble()))
        .arg(m_summaryData.value(QStringLiteral("published")).toInt())
        .arg(m_summaryData.value(QStringLiteral("queued")).toInt()));
    renderChannels();
    renderCatalog();
    renderContent();
    renderAds();
    renderDashboard();
    if (m_searchIndexChanged)
        m_searchIndexChanged();
    if (m_railChanged)
        m_railChanged();
    QJsonObject blogFeed;
    for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray()) {
        const QJsonObject channel = value.toObject();
        if (text(channel, "id") == QLatin1String("blog")) {
            blogFeed = channel.value(QStringLiteral("feed")).toObject();
            break;
        }
    }
    if (blogFeed.value(QStringLiteral("lastReadAt")).toDouble() > 0) {
        setStatus(QString());
    } else {
        setStatus(QStringLiteral("Organization Marketing loaded. Configure a Blog URL, then Refresh to read its RSS feed."));
    }
}

void MarketingFeature::renderChannels()
{
    const int previousRow = m_channelsTable->currentRow();
    QString selectedId = previousRow >= 0 && previousRow < m_channelRows.size()
                             ? m_channelRows.at(previousRow).id : QString();
    bool focusLink = false;
    bool reveal = false;
    for (const ChannelRow &row : std::as_const(m_channelRows)) {
        if (row.profileUrl && row.profileUrl->hasFocus()) {
            selectedId = row.id;
            focusLink = true;
        }
    }
    if (!m_railRow.isEmpty() && isVisible()) {
        selectedId = m_railRow;
        focusLink = m_railRowFocusLink;
        reveal = true;
        m_railRow.clear();
    }
    m_rendering = true;
    m_channelRows.clear();
    m_channelsTable->setRowCount(0);
    for (const QJsonValue &value :
         m_summaryData.value(QStringLiteral("channels")).toArray()) {
        const QJsonObject channel = value.toObject();
        const QString id = text(channel, "channel");
        QJsonObject settings;
        for (const QJsonValue &settingValue :
             m_settings.value(QStringLiteral("channels")).toArray()) {
            const QJsonObject candidate = settingValue.toObject();
            if (text(candidate, "id") == id) {
                settings = candidate;
                break;
            }
        }
        const QJsonObject edit = m_channelEdits.value(id);
        for (auto it = edit.constBegin(); it != edit.constEnd(); ++it) {
            if (it.key() != QLatin1String("accessToken"))
                settings.insert(it.key(), it.value());
        }
        const QString name = channelName(id, m_settings, m_channelCatalog);
        const int row = m_channelsTable->rowCount();
        m_channelsTable->insertRow(row);
        const QStringList values{
            QString::number(qint64(channel.value(QStringLiteral("reach")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("followers")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("impressions")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("engagements")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("clicks")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("views")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("visits")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("indexedPages")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("outbound")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("closedDeals")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("stars")).toDouble())),
            QString::number(qint64(channel.value(QStringLiteral("forks")).toDouble())),
        };

        auto *nameCell = clearWidget();
        auto *nameLayout = new QHBoxLayout(nameCell);
        nameLayout->setContentsMargins(6, 0, 2, 0);
        nameLayout->setSpacing(4);
        auto *nameLabel = new QLabel(QStringLiteral("<a href=\"channel\" style=\"color:%1; text-decoration:none\">%2</a>")
                                         .arg(DashboardColors::from(this).fg.name(), escaped(name)));
        nameLabel->setObjectName(QStringLiteral("marketingChannelName"));
        nameLabel->setTextFormat(Qt::RichText);
        nameLabel->setCursor(Qt::PointingHandCursor);
        nameLabel->setAccessibleName(name);
        connect(nameLabel, &QLabel::linkActivated, this, [this, id] { openChannel(id); });
        auto *refresh = new QPushButton(QStringLiteral("Refresh"));
        refresh->setObjectName(QStringLiteral("marketingLink"));
        refresh->setAccessibleName(name + QStringLiteral(" refresh"));
        auto *capture = new QPushButton(QStringLiteral("Update"));
        capture->setObjectName(QStringLiteral("marketingLink"));
        capture->setAccessibleName(name + QStringLiteral(" update with extension"));
        capture->setToolTip(id == QLatin1String("reddit")
            ? QStringLiteral("Update with the browser extension: opens %1 in your browser, where the ForkMesh extension reads the subreddit's members, weekly visitors and weekly contributions.").arg(name)
            : QStringLiteral("Update with the browser extension: opens %1 in your browser, where the ForkMesh extension reads the channel's page and records its numbers.").arg(name));
        nameLayout->addWidget(nameLabel, 1);
        nameLayout->addWidget(refresh);
        nameLayout->addWidget(capture);
        connect(capture, &QPushButton::clicked, this, [this, id] { captureWithExtension(id); });
        m_channelsTable->setCellWidget(row, 0, nameCell);

        auto *enabled = new QCheckBox;
        enabled->setChecked(settings.value(QStringLiteral("enabled")).toBool(true));
        enabled->setEnabled(m_canManageSettings);
        enabled->setAccessibleName(channelName(id) + QStringLiteral(" active"));
        m_channelsTable->setCellWidget(row, 1, enabled);

        auto *displayName = lineEdit(text(settings, "displayName"));
        displayName->setPlaceholderText(channelName(id));
        displayName->setEnabled(m_canManageSettings);
        displayName->setAccessibleName(channelName(id) + QStringLiteral(" display name"));
        m_channelsTable->setCellWidget(row, 2, displayName);

        auto *profile = clearWidget();
        auto *profileLayout = new QHBoxLayout(profile);
        profileLayout->setContentsMargins(2, 0, 2, 0);
        profileLayout->setSpacing(4);
        auto *profileUrl = lineEdit(text(settings, "profileUrl"));
        profileUrl->setPlaceholderText(channelUrlPlaceholder(id));
        profileUrl->setEnabled(m_canManageSettings);
        profileUrl->setAccessibleName(channelName(id) + QStringLiteral(" channel URL"));
        auto *open = new QPushButton(QStringLiteral("Open ↗"));
        open->setObjectName(QStringLiteral("marketingLink"));
        open->setToolTip(QStringLiteral("Open this channel URL"));
        const auto updateOpen = [profileUrl, open] {
            const QUrl url(normalizedChannelUrl(profileUrl->text()));
            open->setEnabled(url.isValid() && url.scheme() == QLatin1String("https"));
        };
        updateOpen();
        connect(profileUrl, &QLineEdit::textChanged, open,
                [updateOpen](const QString &) { updateOpen(); });
        connect(open, &QPushButton::clicked, this, [profileUrl] {
            const QUrl url(normalizedChannelUrl(profileUrl->text()));
            if (url.isValid() && url.scheme() == QLatin1String("https"))
                QDesktopServices::openUrl(url);
        });
        profileLayout->addWidget(profileUrl, 1);
        profileLayout->addWidget(open);
        m_channelsTable->setCellWidget(row, 3, profile);

        auto *velocity = new VelocityChart;
        m_channelsTable->setCellWidget(row, 4, velocity);

        const bool reddit = id == QLatin1String("reddit");
        for (int column = 5; column < 17; ++column) {
            auto *item = new QTableWidgetItem(values.at(column - 5));
            if (reddit && column == 5)
                item->setToolTip(QStringLiteral("Weekly visitors, read by the browser extension"));
            else if (reddit && column == 6)
                item->setToolTip(QStringLiteral("Members, read by the browser extension"));
            else if (reddit && column == 8)
                item->setToolTip(QStringLiteral("Weekly contributions, read by the browser extension"));
            else if (column == 13)
                item->setToolTip(QStringLiteral("Email sent in the last 30 days, counted from CRM and its Zoho Mail sync"));
            else if (column == 14)
                item->setToolTip(QStringLiteral("CRM contacts at a won stage: won, customer, committed, signed, paid or active"));
            else if (column == 15 || column == 16)
                item->setToolTip(QStringLiteral("GitHub %1 on the linked repository, or summed over the account's own repositories")
                                     .arg(column == 15 ? QStringLiteral("stars") : QStringLiteral("forks")));
            m_channelsTable->setItem(row, column, item);
        }

        const QJsonObject health = channel.value(QStringLiteral("health")).toObject();
        const QString state = text(health, "state");
        const qint64 observedAt = qint64(health.value(QStringLiteral("observedAt")).toDouble());
        const QJsonArray guide = health.value(QStringLiteral("guide")).toArray();
        QStringList guideTip;
        int guideNext = 0;
        for (int index = 0; index < guide.size(); ++index) {
            const QJsonObject guideStep = guide.at(index).toObject();
            const bool done = guideStep.value(QStringLiteral("done")).toBool();
            if (!done && !guideNext)
                guideNext = index + 1;
            guideTip << (done ? QString::fromUtf8("\xE2\x9C\x93 ") : QString::fromUtf8("\xE2\x9C\x97 "))
                            + QStringLiteral("%1. ").arg(index + 1) + text(guideStep, "label");
        }
        QString statusText = observedAt > 0
            ? QStringLiteral("%1 · %2").arg(healthLabel(state), timestamp(observedAt))
            : healthLabel(state);
        if (guideNext && state != QLatin1String("paused"))
            statusText += QStringLiteral(" · get numbers: step %1 of %2").arg(guideNext).arg(guide.size());
        auto *statusItem = new QTableWidgetItem(statusText);
        QStringList statusTip;
        if (guideNext) {
            statusTip << QStringLiteral("Get %1's numbers:").arg(name) << guideTip;
            if (reddit)
                statusTip << QStringLiteral("Refresh only imports Reddit's posts: Reddit gives ForkMesh no member counts, so Update reads them in your browser.");
            statusTip << QString();
        }
        for (const QJsonValue &checkValue : health.value(QStringLiteral("checks")).toArray()) {
            const QJsonObject check = checkValue.toObject();
            statusTip << (check.value(QStringLiteral("ok")).toBool() ? QString::fromUtf8("\xE2\x9C\x93 ")
                                                                     : QString::fromUtf8("\xE2\x9C\x97 "))
                             + text(check, "label");
        }
        if (observedAt > 0)
            statusTip << QStringLiteral("Last numbers from the %1.").arg(sourceLabel(text(health, "source")));
        const QJsonValue change = health.value(QStringLiteral("followersChange"));
        if (change.isDouble())
            statusTip << QStringLiteral("Followers over 30 days: %1%2")
                             .arg(change.toDouble() > 0 ? QStringLiteral("+") : QString())
                             .arg(qint64(change.toDouble()));
        if (health.value(QStringLiteral("engagementRate")).isDouble()) {
            statusTip << QStringLiteral("Engagement rate: %1% (%2)")
                             .arg(health.value(QStringLiteral("engagementRate")).toDouble(), 0, 'f', 1)
                             .arg(engagementFormula(health));
        }
        statusTip << QStringLiteral("Posts in the last 30 days: %1")
                         .arg(health.value(QStringLiteral("postsLast30d")).toInt());
        if (reddit)
            statusTip << QStringLiteral("Members: %1")
                             .arg(qint64(channel.value(QStringLiteral("followers")).toDouble()))
                      << QStringLiteral("Weekly visitors: %1")
                             .arg(qint64(channel.value(QStringLiteral("reach")).toDouble()))
                      << QStringLiteral("Weekly contributions: %1")
                             .arg(qint64(channel.value(QStringLiteral("engagements")).toDouble()));
        if (id == QLatin1String("email"))
            statusTip << QStringLiteral("Emails sent in 30 days: %1")
                             .arg(qint64(channel.value(QStringLiteral("outbound")).toDouble()))
                      << QStringLiteral("Replies in 30 days: %1")
                             .arg(qint64(channel.value(QStringLiteral("replies")).toDouble()))
                      << QStringLiteral("Closed deals: %1 (worth %2)")
                             .arg(qint64(channel.value(QStringLiteral("closedDeals")).toDouble()))
                             .arg(qint64(channel.value(QStringLiteral("closedValue")).toDouble()));
        if (id == QLatin1String("github")) {
            statusTip << QStringLiteral("Stars: %1")
                             .arg(qint64(channel.value(QStringLiteral("stars")).toDouble()))
                      << QStringLiteral("Forks: %1")
                             .arg(qint64(channel.value(QStringLiteral("forks")).toDouble()))
                      << QStringLiteral("Followers: %1")
                             .arg(qint64(channel.value(QStringLiteral("followers")).toDouble()));
            const QJsonValue starsChange = health.value(QStringLiteral("starsChange"));
            if (starsChange.isDouble())
                statusTip << QStringLiteral("Stars over 30 days: %1%2")
                                 .arg(starsChange.toDouble() > 0 ? QStringLiteral("+") : QString())
                                 .arg(qint64(starsChange.toDouble()));
        }
        if (id == QLatin1String("google-search"))
            statusTip << QStringLiteral("Visits from Google in 30 days: %1")
                             .arg(qint64(channel.value(QStringLiteral("visits")).toDouble()))
                      << QStringLiteral("Pages on Google: %1")
                             .arg(qint64(channel.value(QStringLiteral("indexedPages")).toDouble()));
        if (channel.value(QStringLiteral("rating")).toDouble() > 0)
            statusTip << QStringLiteral("Rating: %1 from %2 reviews")
                             .arg(channel.value(QStringLiteral("rating")).toDouble(), 0, 'f', 1)
                             .arg(qint64(channel.value(QStringLiteral("reviews")).toDouble()));
        const QJsonObject playbook =
            m_playbooks.value(channelCategory(id, m_channelCatalog)).toObject();
        if (!playbook.isEmpty()) {
            statusTip << QString() << QStringLiteral("Playbook: ") + text(playbook, "goal");
            QStringList kpis;
            for (const QJsonValue &kpi : playbook.value(QStringLiteral("kpis")).toArray())
                kpis << kpi.toString();
            statusTip << QStringLiteral("Track: ") + kpis.join(QStringLiteral(", "));
            for (const QJsonValue &tip : playbook.value(QStringLiteral("tips")).toArray())
                statusTip << QStringLiteral("• ") + tip.toString();
        }
        statusItem->setToolTip(statusTip.join(QLatin1Char('\n')));
        m_channelsTable->setItem(row, 17, statusItem);

        const QJsonObject feed = settings.value(QStringLiteral("feed")).toObject();
        QStringList details;
        if (!text(feed, "title").isEmpty())
            details << text(feed, "title");
        if (!text(feed, "description").isEmpty())
            details << text(feed, "description");
        if (feed.value(QStringLiteral("lastReadAt")).toDouble() > 0)
            details << QStringLiteral("%1 posts · last refreshed %2")
                           .arg(feed.value(QStringLiteral("itemCount")).toInt())
                           .arg(timestamp(qint64(feed.value(
                               QStringLiteral("lastReadAt")).toDouble())));
        if (!text(feed, "lastError").isEmpty())
            details << QStringLiteral("Last refresh failed: ") +
                           refreshErrorText(id, text(feed, "lastError"),
                                            text(feed, "lastError"));
        details << QStringLiteral("Click for %1's page").arg(name);
        nameLabel->setToolTip(details.join(QLatin1Char('\n')));

        const ChannelRow rowData{id, enabled, displayName, profileUrl, refresh, velocity};
        m_channelRows.append(rowData);
        connect(profileUrl, &QLineEdit::textChanged, profileUrl,
                [this, rowData](const QString &) {
                    recordChannelEdit(rowData);
                    updateRefreshButtons();
                });
        connect(displayName, &QLineEdit::textChanged, displayName,
                [this, rowData](const QString &) { recordChannelEdit(rowData); });
        connect(enabled, &QCheckBox::toggled, enabled, [this, rowData](bool) {
            recordChannelEdit(rowData);
            updateRefreshButtons();
            if (!m_rendering)
                saveChannels();
        });
        // Leaving a field saves it: an edit that only lived in the table was
        // lost by the next reload, and that read as "the link didn't save".
        connect(profileUrl, &QLineEdit::editingFinished, profileUrl, [this, profileUrl] {
            const QString normalized = normalizedChannelUrl(profileUrl->text());
            if (normalized != profileUrl->text())
                profileUrl->setText(normalized);
            if (!m_rendering && !m_channelEdits.isEmpty())
                saveChannels();
        });
        connect(displayName, &QLineEdit::editingFinished, displayName, [this] {
            if (!m_rendering && !m_channelEdits.isEmpty())
                saveChannels();
        });
        connect(refresh, &QPushButton::clicked, this, [this, id] { refreshChannel(id); });
    }
    m_rendering = false;
    updateRefreshButtons();
    updateVelocityCharts();
    updateSaveButton();
    fitTable(m_channelsTable, 18);
    if (!selectedId.isEmpty())
        showChannelRow(selectedId, focusLink, reveal);
}

void MarketingFeature::captureWithExtension(const QString &id)
{
    const QString organization = m_organization->currentData().toString();
    if (organization.isEmpty())
        return;
    QString profileUrl;
    for (const ChannelRow &row : std::as_const(m_channelRows)) {
        if (row.id == id && row.profileUrl)
            profileUrl = normalizedChannelUrl(row.profileUrl->text());
    }
    const QString name = channelName(id, m_settings, m_channelCatalog);
    if (!profileUrl.startsWith(QLatin1String("https://"))) {
        setStatus(QStringLiteral("Add %1's channel URL first: the extension opens that page to read it.").arg(name), true);
        return;
    }
    const auto open = [this, organization, id, name] {
        QUrl url(QString::fromLatin1(kMarketingPage));
        url.setQuery(QStringLiteral("org=%1&page=channels&capture=%2")
                         .arg(QString::fromUtf8(QUrl::toPercentEncoding(organization)),
                              QString::fromUtf8(QUrl::toPercentEncoding(id))));
        QDesktopServices::openUrl(url);
        setStatus(QStringLiteral("Opened %1 in your browser: the ForkMesh extension reads the channel's page there. Press Refresh here once it says updated.").arg(name));
    };
    if (m_channelEdits.isEmpty())
        open();
    else
        saveChannels(open);
}

void MarketingFeature::recordChannelEdit(const ChannelRow &row)
{
    if (m_rendering || !row.enabled || !row.displayName || !row.profileUrl)
        return;
    QJsonObject edit = m_channelEdits.value(row.id);
    edit.insert(QStringLiteral("enabled"), row.enabled->isChecked());
    edit.insert(QStringLiteral("displayName"), row.displayName->text().trimmed());
    edit.insert(QStringLiteral("profileUrl"), normalizedChannelUrl(row.profileUrl->text()));
    QJsonObject saved;
    for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray()) {
        if (text(value.toObject(), "id") == row.id) {
            saved = value.toObject();
            break;
        }
    }
    const bool unchanged = !saved.isEmpty() && !edit.contains(QStringLiteral("accessToken")) &&
        saved.value(QStringLiteral("enabled")).toBool(true) ==
            edit.value(QStringLiteral("enabled")).toBool() &&
        text(saved, "displayName") == text(edit, "displayName") &&
        text(saved, "profileUrl") == text(edit, "profileUrl");
    if (unchanged)
        m_channelEdits.remove(row.id);
    else
        m_channelEdits.insert(row.id, edit);
    updateSaveButton();
}

void MarketingFeature::updateRefreshButtons()
{
    for (const ChannelRow &row : std::as_const(m_channelRows)) {
        if (!row.refresh)
            continue;
        const QString name = channelName(row.id, m_settings, m_channelCatalog);
        const bool busy = m_refreshingChannels.contains(row.id);
        const bool supported = m_refreshableChannels.contains(row.id);
        const bool email = row.id == QLatin1String("email");
        const bool hasUrl = email || (row.profileUrl && !row.profileUrl->text().trimmed().isEmpty());
        const bool active = row.enabled && row.enabled->isChecked();
        row.refresh->setText(busy ? QStringLiteral("Refreshing…") : QStringLiteral("Refresh"));
        row.refresh->setEnabled(m_installed && supported && hasUrl && active && !busy);
        row.refresh->setToolTip(
            !supported ? QStringLiteral("Automatic refresh isn't available for %1 yet. Use Add metrics to log its stats.").arg(name)
            : !hasUrl  ? QStringLiteral("Add a %1 channel URL to refresh it.").arg(name)
            : !active  ? QStringLiteral("%1 is paused.").arg(name)
            : email    ? QStringLiteral("Count email sent in the last 30 days and closed deals from CRM and its Zoho Mail sync.")
            : row.id == QLatin1String("reddit")
                       ? QStringLiteral("Import %1 posts from its public feed. Reddit gives no member counts here: use Update to read them with the browser extension.").arg(name)
                       : QStringLiteral("Read %1 stats and posts now.").arg(name));
        QList<QPointer<QPushButton>> buttons = m_tileRefresh.values(row.id);
        if (row.id == m_detailChannel)
            buttons << m_detailRefresh;
        for (QPushButton *button : std::as_const(buttons)) {
            if (!button)
                continue;
            button->setText(row.refresh->text());
            button->setEnabled(row.refresh->isEnabled());
            button->setToolTip(row.refresh->toolTip());
        }
    }
}

void MarketingFeature::updateVelocityCharts()
{
    struct Window {
        qint64 bucketMs;
        int buckets;
        qint64 rateMs;
        const char *rateUnit;
    };
    const qint64 minute = 60 * 1000LL;
    const qint64 hour = 60 * minute;
    const qint64 day = 24 * hour;
    const Window windows[] = {
        {day, 30, day, "day"},
        {6 * hour, 28, day, "day"},
        {hour, 24, hour, "hr"},
        {minute, 30, hour, "hr"},
    };
    const int selected = m_velocityWindow ? qBound(0, m_velocityWindow->checkedId(), 3) : 0;
    const Window &window = windows[selected];
    // Held as a qsizetype the compiler can see is positive, so sizing the
    // per-channel series below is not a possibly-negative length.
    const qsizetype buckets = qMax<qsizetype>(1, window.buckets);
    const QDateTime current = QDateTime::currentDateTime();
    const qint64 now = current.toMSecsSinceEpoch();
    // Align buckets to local wall-clock boundaries so "Tue", "14:00" and
    // "14:05" tooltips name whole days, hours and minutes.
    const qint64 offset = qint64(current.offsetFromUtc()) * 1000;
    const qint64 end = ((now + offset) / window.bucketMs + 1) * window.bucketMs - offset;
    const qint64 start = end - buckets * window.bucketMs;
    const qint64 span = now - start;

    QHash<QString, QVector<int>> counts;
    for (const QJsonValue &value : std::as_const(m_content)) {
        const QJsonObject item = value.toObject();
        if (text(item, "status") != QLatin1String("published"))
            continue;
        qint64 posted = qint64(item.value(QStringLiteral("publishedAt")).toDouble());
        const qint64 scheduled = qint64(item.value(QStringLiteral("scheduledAt")).toDouble());
        if (posted <= 0)
            posted = scheduled > 0 && scheduled <= now
                ? scheduled : qint64(item.value(QStringLiteral("createdAt")).toDouble());
        if (posted < start || posted >= end || posted > now)
            continue;
        QVector<int> &series = counts[text(item, "channel")];
        if (series.isEmpty())
            series.fill(0, buckets);
        ++series[int((posted - start) / window.bucketMs)];
    }

    for (const ChannelRow &row : std::as_const(m_channelRows)) {
        auto *chart = static_cast<VelocityChart *>(row.velocity);
        if (!chart)
            continue;
        QVector<int> series = counts.value(row.id);
        if (series.isEmpty())
            series.fill(0, buckets);
        int total = 0;
        for (int count : std::as_const(series))
            total += count;
        const double rate = total * double(window.rateMs) / double(qMax<qint64>(1, span));
        const QString summary = total == 0
            ? QStringLiteral("No posts")
            : QStringLiteral("%1 post%2 · %3/%4").arg(total)
                  .arg(total == 1 ? QString() : QStringLiteral("s"))
                  .arg(rate >= 10 ? QString::number(qRound(rate)) : QString::number(rate, 'f', 1))
                  .arg(QLatin1String(window.rateUnit));
        chart->setSeries(channelName(row.id, m_settings, m_channelCatalog), series, start,
                         window.bucketMs, summary);
    }
}

void MarketingFeature::updateSaveButton()
{
    const bool dirty = !m_channelEdits.isEmpty();
    m_saveChannelsButton->setText(dirty ? QStringLiteral("Save channels •")
                                        : QStringLiteral("Save channels"));
    m_saveChannelsButton->setToolTip(dirty ? QStringLiteral("Unsaved channel changes")
                                           : QString());
}

void MarketingFeature::renderCatalog()
{
    m_catalogTable->setRowCount(0);
    for (const QJsonValue &value : m_channelCatalog) {
        const QJsonObject channel = value.toObject();
        const QString id = text(channel, "id");
        bool added = false;
        for (const QJsonValue &settingValue :
             m_settings.value(QStringLiteral("channels")).toArray()) {
            if (text(settingValue.toObject(), "id") == id) {
                added = true;
                break;
            }
        }
        const int row = m_catalogTable->rowCount();
        m_catalogTable->insertRow(row);
        m_catalogTable->setItem(row, 0, new QTableWidgetItem(
            QString::number(channel.value(QStringLiteral("rank")).toInt())));
        m_catalogTable->setItem(row, 1, new QTableWidgetItem(text(channel, "name")));
        m_catalogTable->setItem(row, 2, new QTableWidgetItem(text(channel, "category")));
        auto *button = new QPushButton(added ? QStringLiteral("Added")
                                             : QStringLiteral("Add"));
        button->setEnabled(!added && !m_loading && m_canManageSettings);
        button->setAccessibleName(QStringLiteral("Add %1 to dashboard")
                                      .arg(text(channel, "name")));
        connect(button, &QPushButton::clicked, this,
                [this, id] { addChannel(id); });
        m_catalogTable->setCellWidget(row, 3, button);
    }
    fitTable(m_catalogTable, 10);
}

void MarketingFeature::addChannel(const QString &id)
{
    if (!m_canManageSettings || m_loading)
        return;
    QJsonObject selected;
    for (const QJsonValue &value : m_channelCatalog) {
        const QJsonObject candidate = value.toObject();
        if (text(candidate, "id") == id) {
            selected = candidate;
            break;
        }
    }
    if (selected.isEmpty())
        return;
    QJsonArray channels = channelsPayload();
    for (const QJsonValue &value : channels) {
        if (text(value.toObject(), "id") == id)
            return;
    }
    const QHash<QString, QJsonObject> sent = m_channelEdits;
    channels.append(QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("enabled"), true},
        {QStringLiteral("displayName"), text(selected, "name")},
        {QStringLiteral("profileUrl"), QString()},
    });
    m_loading = true;
    renderCatalog();
    setStatus(QStringLiteral("Adding %1 to the dashboard…")
                  .arg(text(selected, "name")));
    request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")),
            QJsonObject{{QStringLiteral("channels"), channels}},
            [this, sent, name = text(selected, "name")](
                bool ok, const QJsonObject &, const QString &error) {
        m_loading = false;
        if (!ok) {
            renderCatalog();
            return setStatus(QStringLiteral("%1 was not added: %2")
                                 .arg(name, error), true);
        }
        clearSentChannelEdits(sent);
        setStatus(QStringLiteral("%1 was added to the dashboard.").arg(name));
        loadOrganization();
    });
}

void MarketingFeature::deleteChannel(const QString &id)
{
    if (!m_canManageSettings || m_loading)
        return;
    const QString name = channelName(id, m_settings, m_channelCatalog);
    if (QMessageBox::question(
            this, QStringLiteral("Delete channel"),
            QStringLiteral("Delete %1 from the dashboard? Its logged numbers are deleted too. "
                           "Content, ads and posted links stay, and you can add the channel "
                           "again from Add channels.").arg(name))
            != QMessageBox::Yes)
        return;
    m_channelEdits.remove(id);
    updateSaveButton();
    setStatus(QStringLiteral("Deleting %1…").arg(name));
    request(QByteArrayLiteral("DELETE"),
            organizationPath(QStringLiteral("/channels/") + id),
            {}, [this, name](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("%1 was not deleted: %2").arg(name, error), true);
        closeChannel();
        setStatus(QStringLiteral("%1 was deleted.").arg(name));
        loadOrganization();
    });
}

QList<forkmesh::market::NativePackageSearchHit>
MarketingFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty() || !m_installed)
        return hits;
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const auto collect = [&](const QJsonArray &records, const QString &prefix,
                             const char *titleField, const QString &kind,
                             std::initializer_list<const char *> fields) {
        for (const QJsonValue &value : records) {
            if (limit > 0 && hits.size() >= limit)
                return;
            const QJsonObject record = value.toObject();
            const QString channel =
                channelName(text(record, "channel"), m_settings, m_channelCatalog);
            bool matches = channel.contains(needle, Qt::CaseInsensitive);
            for (const char *field : fields)
                matches = matches ||
                          text(record, field).contains(needle, Qt::CaseInsensitive);
            if (!matches)
                continue;
            forkmesh::market::NativePackageSearchHit hit;
            hit.id = prefix + text(record, "id");
            hit.title = text(record, titleField);
            QStringList detail{kind};
            for (const QString &part : {channel, text(record, "status")}) {
                if (!part.isEmpty())
                    detail << part;
            }
            hit.detail = detail.join(dot);
            hits.append(hit);
        }
    };
    collect(m_content, QStringLiteral("content:"), "title", QStringLiteral("Content"),
            {"title", "campaign", "owner", "status", "body", "notes", "url"});
    collect(m_ads, QStringLiteral("ad:"), "name", QStringLiteral("Ad"),
            {"name", "platform", "campaign", "status", "objective", "notes", "url"});
    return hits;
}

void MarketingFeature::activateSearchHit(const QString &id)
{
    const bool ad = id.startsWith(QLatin1String("ad:"));
    const QString recordId = id.mid(id.indexOf(QLatin1Char(':')) + 1);
    QTableWidget *table = ad ? m_adsTable : m_contentTable;
    if (!table || recordId.isEmpty())
        return;
    if (!ad) {
        m_contentSearch->clear();
        m_contentStatus->setCurrentIndex(0);
        m_contentChannel->setCurrentIndex(0);
    } else if (m_adsSearch) {
        m_adChannel.clear();
        {
            const QSignalBlocker blocker(m_adsSearch);
            m_adsSearch->clear();
        }
        m_adsStatus->button(0)->setChecked(true);
        renderAdsPage();
    }
    // Both tables re-select their current row by id when a load rebuilds
    // them, so the selection survives the refresh that opening the page starts.
    for (int row = 0; row < table->rowCount(); ++row) {
        QTableWidgetItem *cell = table->item(row, 0);
        if (!cell || cell->data(Qt::UserRole).toString() != recordId)
            continue;
        reveal(ad ? m_adsPanel : m_contentPanel);
        table->selectRow(row);
        table->scrollToItem(cell);
        return;
    }
}

void MarketingFeature::renderContent()
{
    const QString selectedId = text(selectedContent(), "id");
    const QString chosenChannel = m_contentChannel->currentData().toString();
    {
        const QSignalBlocker blocker(m_contentChannel);
        m_contentChannel->clear();
        m_contentChannel->addItem(QStringLiteral("All channels"), QString());
        QStringList ids;
        for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray())
            ids << text(value.toObject(), "id");
        for (const QJsonValue &value : std::as_const(m_content)) {
            const QString id = text(value.toObject(), "channel");
            if (!id.isEmpty() && !ids.contains(id))
                ids << id;
        }
        for (const QString &id : std::as_const(ids))
            m_contentChannel->addItem(channelName(id, m_settings, m_channelCatalog), id);
        m_contentChannel->setCurrentIndex(qMax(0, m_contentChannel->findData(chosenChannel)));
    }
    const QString needle = m_contentSearch->text().trimmed();
    const QString status = m_contentStatus->currentData().toString();
    const QString channel = m_contentChannel->currentData().toString();
    const DashboardColors colors = DashboardColors::from(this);
    m_contentTable->setRowCount(0);
    int selectedRow = -1;
    for (const QJsonValue &value : std::as_const(m_content)) {
        const QJsonObject item = value.toObject();
        const QString name = channelName(text(item, "channel"), m_settings, m_channelCatalog);
        if ((!status.isEmpty() && text(item, "status") != status) ||
            (!channel.isEmpty() && text(item, "channel") != channel))
            continue;
        if (!needle.isEmpty()) {
            bool found = name.contains(needle, Qt::CaseInsensitive);
            for (const char *field : {"title", "campaign", "owner", "body", "notes"})
                found = found || text(item, field).contains(needle, Qt::CaseInsensitive);
            if (!found)
                continue;
        }
        const int row = m_contentTable->rowCount();
        m_contentTable->insertRow(row);
        const QStringList values{
            text(item, "title"), name,
            channelName(text(item, "status")), text(item, "campaign"), text(item, "owner"),
            timestamp(qint64(item.value(QStringLiteral("scheduledAt")).toDouble())),
            timestamp(qint64(item.value(QStringLiteral("updatedAt")).toDouble())),
        };
        for (int column = 0; column < values.size(); ++column) {
            auto *cell = new QTableWidgetItem(values.at(column));
            if (column == 0) {
                cell->setData(Qt::UserRole, text(item, "id"));
                cell->setToolTip(text(item, "body").left(400));
            }
            if (column == 1)
                cell->setIcon(dotIcon(brandInk(text(item, "channel"), colors)));
            if (column == 2 && text(item, "status") == QLatin1String("published"))
                cell->setForeground(colors.good);
            if (column == 2 && text(item, "status") == QLatin1String("review"))
                cell->setForeground(colors.warn);
            m_contentTable->setItem(row, column, cell);
        }
        if (text(item, "id") == selectedId)
            selectedRow = row;
    }
    if (selectedRow >= 0)
        m_contentTable->selectRow(selectedRow);
    m_contentPanel->hint()->setText(m_contentTable->rowCount() == m_content.size()
        ? QStringLiteral("%1 item%2").arg(m_content.size()).arg(m_content.size() == 1 ? QString() : QStringLiteral("s"))
        : QStringLiteral("%1 of %2").arg(m_contentTable->rowCount()).arg(m_content.size()));
    m_stages->setSelected(m_contentStatus->currentIndex() - 1);
    fitTable(m_contentTable, 12);
}

QJsonObject MarketingFeature::selectedContent() const
{
    const int row = m_contentTable ? m_contentTable->currentRow() : -1;
    auto *cell = row >= 0 ? m_contentTable->item(row, 0) : nullptr;
    const QString id = cell ? cell->data(Qt::UserRole).toString() : QString();
    for (const QJsonValue &value : m_content) {
        if (text(value.toObject(), "id") == id)
            return value.toObject();
    }
    return {};
}

void MarketingFeature::editContent(const QJsonObject &content)
{
    QDialog dialog(this);
    dialog.setWindowTitle(text(content, "id").isEmpty() ? QStringLiteral("New marketing content")
                                                         : QStringLiteral("Edit marketing content"));
    dialog.resize(680, 650);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *title = lineEdit(text(content, "title"));
    auto *channel = new QComboBox;
    const QString currentChannel = text(content, "channel");
    for (const QJsonValue &value :
         m_settings.value(QStringLiteral("channels")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString id = text(item, "id");
        if (item.value(QStringLiteral("enabled")).toBool(true) || id == currentChannel)
            channel->addItem(channelName(id, m_settings, m_channelCatalog), id);
    }
    if (!currentChannel.isEmpty() && channel->findData(currentChannel) < 0)
        channel->addItem(channelName(currentChannel, m_settings, m_channelCatalog),
                         currentChannel);
    channel->setCurrentIndex(qMax(0, channel->findData(text(content, "channel"))));
    auto *status = new QComboBox;
    for (const QString &value : {QStringLiteral("draft"), QStringLiteral("review"),
         QStringLiteral("queued"), QStringLiteral("scheduled"),
         QStringLiteral("published"), QStringLiteral("archived")})
        status->addItem(channelName(value), value);
    status->setCurrentIndex(qMax(0, status->findData(text(content, "status"))));
    auto *campaign = lineEdit(text(content, "campaign"));
    auto *owner = lineEdit(text(content, "owner"));
    auto *url = lineEdit(text(content, "url"));
    auto *scheduled = new QDateTimeEdit;
    scheduled->setCalendarPopup(true);
    scheduled->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    const qint64 scheduledAt = qint64(content.value(QStringLiteral("scheduledAt")).toDouble());
    scheduled->setDateTime(scheduledAt > 0 ? QDateTime::fromMSecsSinceEpoch(scheduledAt)
                                           : QDateTime::currentDateTime());
    QStringList tagValues;
    for (const QJsonValue &tag : content.value(QStringLiteral("tags")).toArray())
        tagValues << tag.toString();
    auto *tagsEdit = lineEdit(tagValues.join(QStringLiteral(", ")));
    auto *body = new QPlainTextEdit(text(content, "body"));
    body->setMinimumHeight(180);
    auto *notes = new QPlainTextEdit(text(content, "notes"));
    form->addRow(QStringLiteral("Title"), title);
    form->addRow(QStringLiteral("Channel"), channel);
    form->addRow(QStringLiteral("Status"), status);
    form->addRow(QStringLiteral("Campaign"), campaign);
    form->addRow(QStringLiteral("Owner"), owner);
    form->addRow(QStringLiteral("Scheduled"), scheduled);
    form->addRow(QStringLiteral("Published URL"), url);
    form->addRow(QStringLiteral("Tags"), tagsEdit);
    form->addRow(QStringLiteral("Copy / outline"), body);
    form->addRow(QStringLiteral("Notes"), notes);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QJsonObject bodyData{
        {QStringLiteral("title"), title->text()},
        {QStringLiteral("channel"), channel->currentData().toString()},
        {QStringLiteral("status"), status->currentData().toString()},
        {QStringLiteral("campaign"), campaign->text()},
        {QStringLiteral("owner"), owner->text()},
        {QStringLiteral("url"), url->text()},
        {QStringLiteral("scheduledAt"), double(scheduled->dateTime().toMSecsSinceEpoch())},
        {QStringLiteral("tags"), QJsonArray::fromStringList(
            tagsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts))},
        {QStringLiteral("body"), body->toPlainText()},
        {QStringLiteral("notes"), notes->toPlainText()},
    };
    const QString id = text(content, "id");
    request(id.isEmpty() ? QByteArrayLiteral("POST") : QByteArrayLiteral("PATCH"),
            organizationPath(QStringLiteral("/content") +
                (id.isEmpty() ? QString() : QLatin1Char('/') + id)), bodyData,
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Content was not saved: %1").arg(error), true);
        setStatus(QStringLiteral("Marketing content saved."));
        loadOrganization();
    });
}

void MarketingFeature::deleteContent()
{
    const QJsonObject content = selectedContent();
    const QString id = text(content, "id");
    if (id.isEmpty() || QMessageBox::question(
            this, QStringLiteral("Delete marketing content"),
            QStringLiteral("Delete %1?").arg(text(content, "title"))) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("DELETE"),
            organizationPath(QStringLiteral("/content/") + id), {},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Content was not deleted: %1").arg(error), true);
        loadOrganization();
    });
}

void MarketingFeature::addMetric(const QString &preset)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add channel metric snapshot"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *channel = new QComboBox;
    for (const QJsonValue &value :
         m_settings.value(QStringLiteral("channels")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString id = text(item, "id");
        if (item.value(QStringLiteral("enabled")).toBool(true))
            channel->addItem(channelName(id, m_settings, m_channelCatalog), id);
    }
    channel->setCurrentIndex(qMax(0, channel->findData(preset)));
    form->addRow(QStringLiteral("Channel"), channel);
    QList<QLineEdit *> edits;
    for (const QString &label : {QStringLiteral("Reach"), QStringLiteral("Followers"),
         QStringLiteral("Impressions"), QStringLiteral("Engagements"),
         QStringLiteral("Clicks"), QStringLiteral("Views"), QStringLiteral("Stars"),
         QStringLiteral("Forks")}) {
        auto *edit = lineEdit(QStringLiteral("0"));
        edit->setValidator(new QIntValidator(0, 2000000000, edit));
        form->addRow(label, edit);
        edits << edit;
    }
    const auto showGitHubRows = [form, channel, edits] {
        const bool github = channel->currentData().toString() == QLatin1String("github");
        for (int index : {6, 7}) {
            edits.at(index)->setVisible(github);
            if (QWidget *label = form->labelForField(edits.at(index)))
                label->setVisible(github);
        }
    };
    showGitHubRows();
    connect(channel, &QComboBox::currentIndexChanged, &dialog, showGitHubRows);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QStringList keys{QStringLiteral("reach"), QStringLiteral("followers"),
        QStringLiteral("impressions"), QStringLiteral("engagements"),
        QStringLiteral("clicks"), QStringLiteral("views"), QStringLiteral("stars"),
        QStringLiteral("forks")};
    QJsonObject body{{QStringLiteral("channel"), channel->currentData().toString()}};
    for (int index = 0; index < keys.size(); ++index)
        body.insert(keys.at(index), edits.at(index)->text().toInt());
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/metrics")),
            body, [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Metrics were not saved: %1").arg(error), true);
        setStatus(QStringLiteral("Metric snapshot saved. Updating dashboard statistics…"));
        loadOrganization();
    });
}

void MarketingFeature::renderAds()
{
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    // Totals stay per currency: amounts in different currencies never add up.
    QStringList totals;
    for (const QJsonValue &value :
         m_adsSummaryData.value(QStringLiteral("currencies")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString currency = text(item, "currency");
        QString line = QStringLiteral("%1 spent of %2 budget")
            .arg(money(item.value(QStringLiteral("spendCents")), currency),
                 money(item.value(QStringLiteral("budgetCents")), currency));
        if (item.value(QStringLiteral("impressions")).toDouble() > 0)
            line += QStringLiteral(" · %1% CTR").arg(
                item.value(QStringLiteral("ctr")).toDouble(), 0, 'f', 2);
        if (item.value(QStringLiteral("conversions")).toDouble() > 0)
            line += QStringLiteral(" · %1 CPA").arg(
                money(item.value(QStringLiteral("cpaCents")), currency));
        if (item.value(QStringLiteral("revenueCents")).toDouble() > 0)
            line += QStringLiteral(" · %1x ROAS").arg(
                item.value(QStringLiteral("roas")).toDouble(), 0, 'f', 2);
        totals << line;
    }
    const int attention = m_adsSummaryData.value(QStringLiteral("attention")).toInt();
    m_adsSummary->setText(totals.isEmpty()
        ? QStringLiteral("No ads yet. New ad plans one for any of the 100 catalog channels.")
        : QStringLiteral("%1 ads · %2 active%3\n%4")
              .arg(m_adsSummaryData.value(QStringLiteral("ads")).toInt())
              .arg(m_adsSummaryData.value(QStringLiteral("active")).toInt())
              .arg(attention > 0
                       ? QStringLiteral(" · %1 off pace or over budget").arg(attention)
                       : QString())
              .arg(totals.join(QLatin1Char('\n'))));

    const QString selectedId = text(selectedAd(), "id");
    m_adsTable->setRowCount(0);
    int selectedRow = -1;
    for (const QJsonValue &value : m_ads) {
        const QJsonObject ad = value.toObject();
        if (!adMatches(ad, true))
            continue;
        const QString currency = text(ad, "currency");
        const QJsonObject pacing = ad.value(QStringLiteral("pacing")).toObject();
        const qint64 startsAt = qint64(ad.value(QStringLiteral("startsAt")).toDouble());
        const qint64 endsAt = qint64(ad.value(QStringLiteral("endsAt")).toDouble());
        const auto day = [](qint64 value) {
            return QDateTime::fromMSecsSinceEpoch(value).toLocalTime()
                .toString(QStringLiteral("yyyy-MM-dd"));
        };
        const QString flight = startsAt <= 0 && endsAt <= 0 ? dash
            : (startsAt > 0 ? day(startsAt) : QStringLiteral("…")) +
              QStringLiteral(" – ") +
              (endsAt > 0 ? day(endsAt) : QStringLiteral("ongoing"));
        const double budgetTotal = pacing.value(QStringLiteral("totalBudgetCents")).toDouble();
        const double budget = ad.value(QStringLiteral("budgetCents")).toDouble();
        const QString budgetText = budgetTotal > 0
            ? money(budgetTotal, currency)
            : budget > 0 ? money(budget, currency) + QStringLiteral(" / day") : dash;
        const bool hasImpressions = ad.value(QStringLiteral("impressions")).toDouble() > 0;
        const bool hasConversions = ad.value(QStringLiteral("conversions")).toDouble() > 0;
        const bool hasReturn = ad.value(QStringLiteral("spendCents")).toDouble() > 0 &&
                               ad.value(QStringLiteral("revenueCents")).toDouble() > 0;
        const QStringList values{
            text(ad, "name"),
            channelName(text(ad, "channel"), m_settings, m_channelCatalog),
            text(ad, "platform").isEmpty() ? QStringLiteral("Booked directly")
                                           : text(ad, "platform"),
            adLabel(text(ad, "status")), flight, budgetText,
            money(ad.value(QStringLiteral("spendCents")), currency),
            adPacingText(text(pacing, "state")),
            hasImpressions ? count(ad.value(QStringLiteral("impressions"))) : dash,
            hasImpressions ? QStringLiteral("%1%").arg(
                ad.value(QStringLiteral("ctr")).toDouble(), 0, 'f', 2) : dash,
            hasConversions ? count(ad.value(QStringLiteral("conversions"))) : dash,
            hasConversions ? money(ad.value(QStringLiteral("cpaCents")), currency) : dash,
            hasReturn ? QStringLiteral("%1x").arg(
                ad.value(QStringLiteral("roas")).toDouble(), 0, 'f', 2) : dash,
        };
        const int row = m_adsTable->rowCount();
        m_adsTable->insertRow(row);
        for (int column = 0; column < values.size(); ++column) {
            auto *cell = new QTableWidgetItem(values.at(column));
            if (column == 0) {
                cell->setData(Qt::UserRole, text(ad, "id"));
                cell->setToolTip(QStringList{adLabel(text(ad, "objective")),
                                             text(ad, "campaign"), text(ad, "headline")}
                                     .filter(QRegularExpression(QStringLiteral(".")))
                                     .join(QStringLiteral(" · ")));
            }
            if (column >= 5)
                cell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_adsTable->setItem(row, column, cell);
        }
        if (text(ad, "id") == selectedId)
            selectedRow = row;
    }
    if (selectedRow >= 0)
        m_adsTable->selectRow(selectedRow);
    fitTable(m_adsTable, 10);
}

QJsonObject MarketingFeature::selectedAd() const
{
    const int row = m_adsTable ? m_adsTable->currentRow() : -1;
    auto *cell = row >= 0 ? m_adsTable->item(row, 0) : nullptr;
    const QString id = cell ? cell->data(Qt::UserRole).toString() : QString();
    for (const QJsonValue &value : m_ads) {
        if (!id.isEmpty() && text(value.toObject(), "id") == id)
            return value.toObject();
    }
    return {};
}

void MarketingFeature::saveAd(const QString &id, const QJsonObject &body,
                              const QString &done)
{
    request(id.isEmpty() ? QByteArrayLiteral("POST") : QByteArrayLiteral("PATCH"),
            organizationPath(QStringLiteral("/ads") +
                (id.isEmpty() ? QString() : QLatin1Char('/') + id)), body,
            [this, done](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The ad was not saved: %1")
                                 .arg(adErrorText(payload, error)), true);
        setStatus(done);
        loadOrganization();
    });
}

void MarketingFeature::editAd(const QJsonObject &ad)
{
    QDialog dialog(this);
    dialog.setWindowTitle(ad.isEmpty() ? QStringLiteral("New ad")
                                       : QStringLiteral("Edit ad"));
    dialog.resize(680, 760);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *name = lineEdit(text(ad, "name"));
    // Any catalog channel takes an ad; the dashboard's own channels lead.
    auto *channel = new QComboBox;
    for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray()) {
        const QString id = text(value.toObject(), "id");
        if (value.toObject().value(QStringLiteral("enabled")).toBool(true))
            channel->addItem(channelName(id, m_settings, m_channelCatalog), id);
    }
    if (channel->count() > 0)
        channel->insertSeparator(channel->count());
    for (const QJsonValue &value : m_channelCatalog) {
        const QString id = text(value.toObject(), "id");
        if (channel->findData(id) < 0)
            channel->addItem(channelName(id, m_settings, m_channelCatalog), id);
    }
    channel->setCurrentIndex(qMax(0, channel->findData(text(ad, "channel"))));
    const auto choices = [](const QStringList &values, const QString &current) {
        auto *combo = new QComboBox;
        for (const QString &value : values)
            combo->addItem(adLabel(value), value);
        combo->setCurrentIndex(qMax(0, combo->findData(current)));
        return combo;
    };
    auto *status = choices({QStringLiteral("draft"), QStringLiteral("review"),
        QStringLiteral("scheduled"), QStringLiteral("active"), QStringLiteral("paused"),
        QStringLiteral("completed"), QStringLiteral("archived")}, text(ad, "status"));
    auto *objective = choices({QStringLiteral("awareness"), QStringLiteral("traffic"),
        QStringLiteral("engagement"), QStringLiteral("video-views"),
        QStringLiteral("leads"), QStringLiteral("app-installs"),
        QStringLiteral("conversions"), QStringLiteral("sales")}, text(ad, "objective"));
    auto *campaign = lineEdit(text(ad, "campaign"));
    auto *owner = lineEdit(text(ad, "owner"));
    auto *budget = new QDoubleSpinBox;
    budget->setDecimals(2);
    budget->setRange(0, 1e12);
    budget->setGroupSeparatorShown(true);
    budget->setValue(ad.value(QStringLiteral("budgetCents")).toDouble() / 100.0);
    auto *budgetType = choices({QStringLiteral("lifetime"), QStringLiteral("daily")},
                               text(ad, "budgetType"));
    QString defaultCurrency = QStringLiteral("USD");
    const QJsonArray currencies = m_adsSummaryData.value(QStringLiteral("currencies")).toArray();
    if (!currencies.isEmpty())
        defaultCurrency = text(currencies.first().toObject(), "currency");
    auto *currency = lineEdit(ad.isEmpty() ? defaultCurrency : text(ad, "currency"));
    currency->setMaxLength(3);
    currency->setPlaceholderText(QStringLiteral("USD"));
    auto *budgetRow = new QHBoxLayout;
    budgetRow->addWidget(budget, 1);
    budgetRow->addWidget(budgetType);
    budgetRow->addWidget(currency);
    // A flight date is optional, which a bare QDateTimeEdit cannot express.
    const auto flightDate = [&ad](const char *key, QCheckBox *&enabled) {
        const qint64 value = qint64(ad.value(QLatin1String(key)).toDouble());
        auto *row = new QHBoxLayout;
        enabled = new QCheckBox(QStringLiteral("Set"));
        enabled->setChecked(value > 0);
        auto *edit = new QDateTimeEdit(value > 0 ? QDateTime::fromMSecsSinceEpoch(value)
                                                 : QDateTime::currentDateTime());
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
        edit->setEnabled(value > 0);
        QObject::connect(enabled, &QCheckBox::toggled, edit, &QWidget::setEnabled);
        row->addWidget(enabled);
        row->addWidget(edit, 1);
        return std::pair<QHBoxLayout *, QDateTimeEdit *>(row, edit);
    };
    QCheckBox *hasStart = nullptr;
    QCheckBox *hasEnd = nullptr;
    const auto starts = flightDate("startsAt", hasStart);
    const auto ends = flightDate("endsAt", hasEnd);
    auto *audience = lineEdit(text(ad, "audience"));
    auto *headline = lineEdit(text(ad, "headline"));
    auto *body = new QPlainTextEdit(text(ad, "body"));
    body->setMinimumHeight(120);
    auto *destination = lineEdit(text(ad, "destinationUrl"));
    destination->setPlaceholderText(QStringLiteral("https://landing-page"));
    auto *manager = lineEdit(text(ad, "managerUrl"));
    manager->setPlaceholderText(QStringLiteral("https://link-to-this-ad-in-its-console"));
    QStringList tagValues;
    for (const QJsonValue &tag : ad.value(QStringLiteral("tags")).toArray())
        tagValues << tag.toString();
    auto *tagsEdit = lineEdit(tagValues.join(QStringLiteral(", ")));
    auto *notes = new QPlainTextEdit(text(ad, "notes"));
    form->addRow(QStringLiteral("Ad name"), name);
    form->addRow(QStringLiteral("Channel"), channel);
    form->addRow(QStringLiteral("Status"), status);
    form->addRow(QStringLiteral("Objective"), objective);
    form->addRow(QStringLiteral("Campaign"), campaign);
    form->addRow(QStringLiteral("Owner"), owner);
    form->addRow(QStringLiteral("Budget"), budgetRow);
    form->addRow(QStringLiteral("Starts"), starts.first);
    form->addRow(QStringLiteral("Ends"), ends.first);
    form->addRow(QStringLiteral("Audience / targeting"), audience);
    form->addRow(QStringLiteral("Headline"), headline);
    form->addRow(QStringLiteral("Ad copy / brief"), body);
    form->addRow(QStringLiteral("Landing page URL"), destination);
    form->addRow(QStringLiteral("Console link"), manager);
    form->addRow(QStringLiteral("Tags"), tagsEdit);
    form->addRow(QStringLiteral("Notes"), notes);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString currencyCode = currency->text().trimmed().toUpper();
    saveAd(text(ad, "id"), QJsonObject{
        {QStringLiteral("name"), name->text()},
        {QStringLiteral("channel"), channel->currentData().toString()},
        {QStringLiteral("status"), status->currentData().toString()},
        {QStringLiteral("objective"), objective->currentData().toString()},
        {QStringLiteral("campaign"), campaign->text()},
        {QStringLiteral("owner"), owner->text()},
        {QStringLiteral("budgetCents"), double(qRound64(budget->value() * 100.0))},
        {QStringLiteral("budgetType"), budgetType->currentData().toString()},
        {QStringLiteral("currency"),
         currencyCode.isEmpty() ? QStringLiteral("USD") : currencyCode},
        {QStringLiteral("startsAt"), hasStart->isChecked()
             ? double(starts.second->dateTime().toMSecsSinceEpoch()) : 0.0},
        {QStringLiteral("endsAt"), hasEnd->isChecked()
             ? double(ends.second->dateTime().toMSecsSinceEpoch()) : 0.0},
        {QStringLiteral("audience"), audience->text()},
        {QStringLiteral("headline"), headline->text()},
        {QStringLiteral("body"), body->toPlainText()},
        {QStringLiteral("destinationUrl"), destination->text()},
        {QStringLiteral("managerUrl"), manager->text()},
        {QStringLiteral("tags"), QJsonArray::fromStringList(
            tagsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts))},
        {QStringLiteral("notes"), notes->toPlainText()},
    }, QStringLiteral("Ad saved."));
}

void MarketingFeature::logAdResults()
{
    const QJsonObject ad = selectedAd();
    if (ad.isEmpty())
        return setStatus(QStringLiteral("Select an ad to log its results."), true);
    const QString currency = text(ad, "currency");
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Log ad results"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *note = new QLabel(QStringLiteral("%1 — totals to date, as %2 reports them.")
        .arg(text(ad, "name"),
             text(ad, "platform").isEmpty() ? QStringLiteral("the channel")
                                            : text(ad, "platform")));
    note->setObjectName(QStringLiteral("mutedLabel"));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *form = new QFormLayout;
    const auto amount = [&ad](const char *key) {
        auto *edit = new QDoubleSpinBox;
        edit->setDecimals(2);
        edit->setRange(0, 1e12);
        edit->setGroupSeparatorShown(true);
        edit->setValue(ad.value(QLatin1String(key)).toDouble() / 100.0);
        return edit;
    };
    auto *spend = amount("spendCents");
    auto *revenue = amount("revenueCents");
    form->addRow(QStringLiteral("Spend to date (%1)").arg(currency), spend);
    form->addRow(QStringLiteral("Revenue attributed (%1)").arg(currency), revenue);
    const QStringList keys{QStringLiteral("impressions"), QStringLiteral("clicks"),
                           QStringLiteral("conversions")};
    QList<QDoubleSpinBox *> counters;
    for (const QString &key : keys) {
        auto *edit = new QDoubleSpinBox;
        edit->setDecimals(0);
        edit->setRange(0, 1e15);
        edit->setGroupSeparatorShown(true);
        edit->setValue(ad.value(key).toDouble());
        form->addRow(adLabel(key), edit);
        counters << edit;
    }
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QJsonObject body{
        {QStringLiteral("spendCents"), double(qRound64(spend->value() * 100.0))},
        {QStringLiteral("revenueCents"), double(qRound64(revenue->value() * 100.0))},
    };
    for (int index = 0; index < keys.size(); ++index)
        body.insert(keys.at(index), double(qRound64(counters.at(index)->value())));
    saveAd(text(ad, "id"), body, QStringLiteral("Ad results saved."));
}

void MarketingFeature::deleteAd()
{
    const QJsonObject ad = selectedAd();
    const QString id = text(ad, "id");
    if (id.isEmpty() || QMessageBox::question(
            this, QStringLiteral("Delete ad"),
            QStringLiteral("Delete %1 and its logged results?").arg(text(ad, "name")))
            != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/ads/") + id), {},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The ad was not deleted: %1").arg(error), true);
        setStatus(QStringLiteral("Ad deleted."));
        loadOrganization();
    });
}

void MarketingFeature::openAdConsole()
{
    const QJsonObject ad = selectedAd();
    if (ad.isEmpty())
        return setStatus(QStringLiteral("Select an ad to open its console."), true);
    const QString link = text(ad, "managerUrl").isEmpty() ? text(ad, "platformUrl")
                                                          : text(ad, "managerUrl");
    const QUrl url(link);
    if (!url.isValid() || url.scheme() != QLatin1String("https"))
        return setStatus(QStringLiteral("%1 has no self-serve ad console. Add a console link to the ad to open it from here.")
                             .arg(channelName(text(ad, "channel"), m_settings,
                                              m_channelCatalog)), true);
    QDesktopServices::openUrl(url);
}

QJsonArray MarketingFeature::channelsPayload() const
{
    QJsonArray channels;
    QSet<QString> seen;
    const auto merged = [this](QJsonObject channel, const QString &id) {
        const QJsonObject edit = m_channelEdits.value(id);
        for (auto it = edit.constBegin(); it != edit.constEnd(); ++it)
            channel.insert(it.key(), it.value());
        channel.remove(QStringLiteral("hasAccessToken"));
        return channel;
    };
    for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray()) {
        const QString id = text(value.toObject(), "id");
        seen.insert(id);
        channels.append(merged(value.toObject(), id));
    }
    // A row shown only because it has content or metrics becomes configured
    // once someone edits it.
    for (const ChannelRow &row : m_channelRows) {
        if (!seen.contains(row.id) && m_channelEdits.contains(row.id)) {
            seen.insert(row.id);
            channels.append(merged(QJsonObject{{QStringLiteral("id"), row.id}}, row.id));
        }
    }
    return channels;
}

void MarketingFeature::clearSentChannelEdits(const QHash<QString, QJsonObject> &sent)
{
    // Keep anything typed after the request left; it still needs a save.
    for (auto it = sent.constBegin(); it != sent.constEnd(); ++it) {
        if (m_channelEdits.value(it.key()) == it.value())
            m_channelEdits.remove(it.key());
    }
    updateSaveButton();
}

void MarketingFeature::saveChannels(std::function<void()> then)
{
    if (!m_canManageSettings)
        return;
    if (then)
        m_afterSave.append(std::move(then));
    if (m_savingChannels) {
        m_saveQueued = true;
        return;
    }
    m_savingChannels = true;
    const QHash<QString, QJsonObject> sent = m_channelEdits;
    m_saveChannelsButton->setEnabled(false);
    setStatus(QStringLiteral("Saving channel settings…"));
    request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")),
            QJsonObject{{QStringLiteral("channels"), channelsPayload()}},
            [this, sent](bool ok, const QJsonObject &payload, const QString &error) {
        m_savingChannels = false;
        m_saveChannelsButton->setEnabled(m_canManageSettings);
        if (!ok) {
            m_saveQueued = false;
            m_afterSave.clear();
            if (text(payload, "error") == QLatin1String("invalid_channel_url"))
                return setStatus(QStringLiteral("Channels were not saved: the %1 URL must be an https:// address.")
                                     .arg(channelName(text(payload, "channel"), m_settings,
                                                      m_channelCatalog)), true);
            return setStatus(QStringLiteral("Channels were not saved: %1").arg(error), true);
        }
        const QJsonObject saved = payload.value(QStringLiteral("settings")).toObject();
        if (!saved.isEmpty())
            m_settings = saved;
        clearSentChannelEdits(sent);
        if (m_saveQueued) {
            m_saveQueued = false;
            saveChannels();
            return;
        }
        setStatus(QStringLiteral("Channel settings saved."));
        updateRefreshButtons();
        const QList<std::function<void()>> callbacks = std::exchange(m_afterSave, {});
        for (const std::function<void()> &callback : callbacks)
            callback();
    });
}

QString MarketingFeature::railTitle() const
{
    return QStringLiteral("Channels");
}

QList<forkmesh::market::NativePackageRailTile> MarketingFeature::railTiles() const
{
    QList<forkmesh::market::NativePackageRailTile> tiles;
    QList<forkmesh::market::NativePackageRailTile> paused;
    const auto placeholder = [this] {
        forkmesh::market::NativePackageRailTile tile;
        tile.id = QStringLiteral("marketing");
        tile.label = QStringLiteral("Marketing");
        tile.count = QString::fromUtf8("\xE2\x80\x94");
        tile.countLabel = QStringLiteral("no channels yet");
        tile.opensPage = true;
        if (!m_railProblem.isEmpty()) {
            tile.stateLabel = m_railProblem;
            tile.details << (m_railProblem == QLatin1String("Could not load")
                ? QStringLiteral("Marketing could not read the channels. It keeps trying in the background, or open Marketing to retry now.")
                : QStringLiteral("Join or create an organization to track its marketing channels."));
        } else if (!m_installed && !m_organization->currentData().toString().isEmpty()) {
            tile.stateLabel = QStringLiteral("Not installed for this organization");
            tile.details << QStringLiteral("Open Marketing to install it for this organization and track its channels.");
        } else if (m_installed && m_summaryData.contains(QStringLiteral("channels"))) {
            tile.stateLabel = QStringLiteral("No channels turned on");
            tile.details << QStringLiteral("Open Marketing to turn on a channel and see its status here.");
        } else {
            tile.stateLabel = QStringLiteral("Loading channels");
            tile.details << QStringLiteral("Reading the channels from your organization.");
        }
        return QList<forkmesh::market::NativePackageRailTile>{tile};
    };
    if (!m_installed)
        return placeholder();
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    for (const QJsonValue &value :
         m_summaryData.value(QStringLiteral("channels")).toArray()) {
        const QJsonObject channel = value.toObject();
        const QString id = text(channel, "channel");
        if (id.isEmpty())
            continue;
        QJsonObject settings;
        for (const QJsonValue &settingValue :
             m_settings.value(QStringLiteral("channels")).toArray()) {
            if (text(settingValue.toObject(), "id") == id) {
                settings = settingValue.toObject();
                break;
            }
        }
        const QJsonObject health = channel.value(QStringLiteral("health")).toObject();
        const QString state = text(health, "state");
        const QString name = channelName(id, m_settings, m_channelCatalog);
        const QString link = normalizedChannelUrl(text(settings, "profileUrl"));
        const bool hasLink = link.startsWith(QLatin1String("https://"));
        const qint64 followers = qint64(channel.value(QStringLiteral("followers")).toDouble());
        const qint64 views = qint64(channel.value(QStringLiteral("views")).toDouble());
        const qint64 reach = qint64(channel.value(QStringLiteral("reach")).toDouble());
        const qint64 visits = qint64(channel.value(QStringLiteral("visits")).toDouble());
        const qint64 indexedPages = qint64(channel.value(QStringLiteral("indexedPages")).toDouble());
        const bool google = id == QLatin1String("google-search");
        const bool reddit = id == QLatin1String("reddit");
        const bool email = id == QLatin1String("email");
        const qint64 outbound = qint64(channel.value(QStringLiteral("outbound")).toDouble());
        const qint64 closedDeals = qint64(channel.value(QStringLiteral("closedDeals")).toDouble());
        const qint64 closedValue = qint64(channel.value(QStringLiteral("closedValue")).toDouble());
        const bool github = id == QLatin1String("github");
        const qint64 stars = qint64(channel.value(QStringLiteral("stars")).toDouble());
        const qint64 forks = qint64(channel.value(QStringLiteral("forks")).toDouble());
        const qint64 engagements = qint64(channel.value(QStringLiteral("engagements")).toDouble());
        const QJsonArray guide = health.value(QStringLiteral("guide")).toArray();
        const int posts = health.value(QStringLiteral("postsLast30d")).toInt();
        const int cadence = qMax(1, health.value(QStringLiteral("cadenceDays")).toInt());
        const QJsonObject playbook =
            m_playbooks.value(channelCategory(id, m_channelCatalog)).toObject();
        const QJsonArray tips = playbook.value(QStringLiteral("tips")).toArray();

        forkmesh::market::NativePackageRailTile tile;
        tile.id = id;
        tile.label = name;
        tile.opensPage = true;
        tile.tone = railTone(state);
        tile.stateLabel = healthLabel(state);
        if (email && (outbound > 0 || closedDeals > 0)) {
            tile.count = compactCount(outbound);
            tile.countLabel = outbound == 1 ? QStringLiteral("email sent in 30 days")
                                            : QStringLiteral("emails sent in 30 days");
        } else if (github && stars > 0) {
            tile.count = compactCount(stars);
            tile.countLabel = stars == 1 ? QStringLiteral("star") : QStringLiteral("stars");
        } else if (followers > 0) {
            tile.count = compactCount(followers);
            tile.countLabel = reddit ? (followers == 1 ? QStringLiteral("member") : QStringLiteral("members"))
                                     : (followers == 1 ? QStringLiteral("follower") : QStringLiteral("followers"));
        } else if (views > 0) {
            tile.count = compactCount(views);
            tile.countLabel = views == 1 ? QStringLiteral("view") : QStringLiteral("views");
        } else if (visits > 0) {
            tile.count = compactCount(visits);
            tile.countLabel = visits == 1 ? QStringLiteral("visit in 30 days")
                                          : QStringLiteral("visits in 30 days");
        } else if (indexedPages > 0) {
            tile.count = compactCount(indexedPages);
            tile.countLabel = QStringLiteral("pages on Google");
        } else if (reach > 0) {
            tile.count = compactCount(reach);
            tile.countLabel = QStringLiteral("reach");
        } else if (posts > 0) {
            tile.count = QString::number(posts);
            tile.countLabel = posts == 1 ? QStringLiteral("post in 30 days")
                                         : QStringLiteral("posts in 30 days");
        } else {
            tile.count = QString::fromUtf8("\xE2\x80\x94");
            tile.countLabel = QStringLiteral("no numbers yet");
        }

        QStringList numbers;
        if (github && (stars > 0 || forks > 0)) {
            QString line = QStringLiteral("%1 stars").arg(count(stars));
            const QJsonValue change = health.value(QStringLiteral("starsChange"));
            if (change.isDouble())
                line += QStringLiteral(" (%1%2 in 30 days)")
                            .arg(change.toDouble() > 0 ? QStringLiteral("+") : QString())
                            .arg(QLocale().toString(qint64(change.toDouble())));
            numbers << line << QStringLiteral("%1 forks").arg(count(forks));
        }
        if (followers > 0) {
            QString line = (reddit ? QStringLiteral("%1 members") : QStringLiteral("%1 followers")).arg(count(followers));
            const QJsonValue change = health.value(QStringLiteral("followersChange"));
            if (change.isDouble())
                line += QStringLiteral(" (%1%2 in 30 days)")
                            .arg(change.toDouble() > 0 ? QStringLiteral("+") : QString())
                            .arg(QLocale().toString(qint64(change.toDouble())));
            numbers << line;
        }
        if (reddit && reach > 0)
            numbers << QStringLiteral("%1 weekly visitors").arg(count(reach));
        if (reddit && engagements > 0)
            numbers << QStringLiteral("%1 weekly contributions").arg(count(engagements));
        if (views > 0)
            numbers << QStringLiteral("%1 views").arg(count(views));
        if (visits > 0)
            numbers << QStringLiteral("%1 visits in 30 days").arg(count(visits));
        if (indexedPages > 0)
            numbers << QStringLiteral("%1 pages on Google").arg(count(indexedPages));
        if (email && (outbound > 0 || closedDeals > 0)) {
            numbers << QStringLiteral("%1 emails sent in 30 days").arg(count(outbound));
            numbers << (closedValue > 0
                ? QStringLiteral("%1 closed deals worth %2").arg(count(closedDeals), count(closedValue))
                : QStringLiteral("%1 closed deals").arg(count(closedDeals)));
        }
        if (health.value(QStringLiteral("engagementRate")).isDouble()) {
            numbers << QStringLiteral("%1% engagement %2")
                           .arg(health.value(QStringLiteral("engagementRate")).toDouble(), 0, 'f', 1)
                           .arg(engagementBaseLabel(health));
        }
        if (followers > 0 || views > 0 || reach > 0)
            numbers << QStringLiteral("%1 post%2 in 30 days").arg(posts).arg(posts == 1 ? QString() : QStringLiteral("s"));
        if (!numbers.isEmpty())
            tile.details << numbers.join(dot);
        const QJsonValue age = health.value(QStringLiteral("ageDays"));
        tile.details << (age.isDouble()
            ? QStringLiteral("Numbers from %1 (%2)")
                  .arg(agoText(qint64(age.toDouble())), sourceLabel(text(health, "source")))
            : QStringLiteral("No numbers recorded yet"));
        if (!text(playbook, "goal").isEmpty())
            tile.details << QStringLiteral("Goal: ") + text(playbook, "goal");

        QHash<QString, bool> checks;
        for (const QJsonValue &checkValue : health.value(QStringLiteral("checks")).toArray()) {
            const QJsonObject check = checkValue.toObject();
            const bool ok = check.value(QStringLiteral("ok")).toBool();
            checks.insert(text(check, "id"), ok);
            tile.pending += ok ? 0 : 1;
        }
        const auto step = [&tile](const char *stepId, const QString &label) {
            tile.steps.append({QLatin1String(stepId), label});
        };
        if (state == QLatin1String("paused")) {
            tile.pending = 0;
            step(m_canManageSettings ? "enable" : "row",
                 m_canManageSettings ? QStringLiteral("Turn %1 back on").arg(name)
                                     : QStringLiteral("Ask an owner or admin to turn %1 back on").arg(name));
        } else {
            if (!checks.value(QStringLiteral("link"), true))
                step("link", m_canManageSettings
                                 ? QStringLiteral("Add the %1 link so its numbers can be read").arg(name)
                                 : QStringLiteral("Ask an owner or admin to add the %1 link").arg(name));
            if (!checks.value(QStringLiteral("fresh"), true)) {
                const QString when = age.isDouble()
                    ? QStringLiteral("last read %1").arg(agoText(qint64(age.toDouble())))
                    : QStringLiteral("never read");
                if (email) {
                    for (const QJsonValue &guideValue : guide) {
                        const QJsonObject guideStep = guideValue.toObject();
                        if (guideStep.value(QStringLiteral("done")).toBool())
                            continue;
                        const QString action = text(guideStep, "action");
                        step(action == QLatin1String("refresh") ? "refresh" : "row",
                             action == QLatin1String("refresh") ? text(guideStep, "label")
                                 : text(guideStep, "label") + QStringLiteral(" (open CRM from the sidebar)"));
                    }
                } else if (hasLink && !guide.isEmpty()) {
                    for (const QJsonValue &guideValue : guide) {
                        const QJsonObject guideStep = guideValue.toObject();
                        if (guideStep.value(QStringLiteral("done")).toBool())
                            continue;
                        const bool link = text(guideStep, "action") == QLatin1String("link");
                        if (link && !m_canManageSettings)
                            continue;
                        step(link ? "link" : "capture", text(guideStep, "label"));
                    }
                } else if (hasLink && google) {
                    step("refresh", QStringLiteral("Refresh visits from Google (%1)").arg(when));
                    step("capture", QStringLiteral("Count pages on Google with the browser extension"));
                } else if (hasLink && m_refreshableChannels.contains(id))
                    step("refresh", QStringLiteral("Refresh the numbers (%1)").arg(when));
                else if (hasLink)
                    step("capture", QStringLiteral("Update the numbers with the browser extension (%1)").arg(when));
                step("metrics", QStringLiteral("Log the numbers by hand"));
            }
            if (!checks.value(QStringLiteral("posting"), true))
                step("post", QStringLiteral("Post on %1: nothing in the last %2 days").arg(name).arg(cadence * 2));
            if (checks.contains(QStringLiteral("growing")) && !checks.value(QStringLiteral("growing"))) {
                const QString tip = tips.isEmpty() ? QString() : tips.at(0).toString();
                step("row", tip.isEmpty()
                                ? QStringLiteral("The audience shrank over 30 days: review what changed")
                                : QStringLiteral("The audience shrank over 30 days. %1").arg(tip));
            }
            if (!checks.value(QStringLiteral("zoho"), true) && checks.value(QStringLiteral("fresh"), true))
                step("row", QStringLiteral("Connect Zoho Mail in CRM so mail sent from Zoho counts (open CRM from the sidebar)"));
            if (!checks.value(QStringLiteral("outbound"), true))
                step("refresh", QStringLiteral("No email sent in 30 days: send from CRM or Zoho Mail, then Refresh"));
            if (tile.steps.isEmpty() && email) {
                step("refresh", QStringLiteral("Refresh outbound email and closed deals from CRM"));
            } else if (tile.steps.isEmpty() && google) {
                step("refresh", QStringLiteral("Refresh visits from Google"));
                step("capture", QStringLiteral("Count pages on Google with the browser extension"));
                if (!tips.isEmpty())
                    step("row", tips.at(posts % tips.size()).toString());
            } else if (tile.steps.isEmpty()) {
                step("post", QStringLiteral("Plan the next post (every %1 day%2)")
                                 .arg(cadence).arg(cadence == 1 ? QString() : QStringLiteral("s")));
                if (!tips.isEmpty())
                    step("row", tips.at(posts % tips.size()).toString());
            }
        }
        if (hasLink)
            step("open", QStringLiteral("Open %1").arg(name) + QString::fromUtf8(" \xE2\x86\x97"));
        (state == QLatin1String("paused") ? paused : tiles).append(tile);
    }
    if (tiles.isEmpty() && paused.isEmpty())
        return placeholder();
    return tiles + paused;
}

void MarketingFeature::prepareRail()
{
    if (m_loading || isVisible())
        return;
    refresh();
}

void MarketingFeature::showChannelRow(const QString &id, bool focusLink, bool reveal)
{
    for (int row = 0; row < m_channelRows.size(); ++row) {
        const ChannelRow &channel = m_channelRows.at(row);
        if (channel.id != id)
            continue;
        if (reveal)
            this->reveal(m_channelsPanel);
        m_channelsTable->selectRow(row);
        if (reveal)
            m_channelsTable->scrollTo(m_channelsTable->model()->index(row, 0));
        if (focusLink && channel.profileUrl && channel.profileUrl->isEnabled())
            channel.profileUrl->setFocus(Qt::OtherFocusReason);
        return;
    }
}

void MarketingFeature::openRailTile(const QString &tile)
{
    if (tile == QLatin1String("marketing"))
        return;
    openChannel(tile);
}

void MarketingFeature::openChannel(const QString &id)
{
    if (id.isEmpty() || !m_detailScroll)
        return;
    const bool opened = m_detailChannel != id;
    if (opened)
        m_detailScroll->verticalScrollBar()->setValue(0);
    m_detailChannel = id;
    renderChannelDetail();
    applyView();
    if (opened && id == QLatin1String("reddit") && m_performance.isEmpty())
        loadPerformance();
}

void MarketingFeature::closeChannel()
{
    if (m_detailChannel.isEmpty())
        return;
    m_detailChannel.clear();
    m_detailRefresh.clear();
    applyView();
}

void MarketingFeature::applyView()
{
    const bool detail = m_installed && !m_detailChannel.isEmpty();
    for (int index = 0; index < m_pages.size(); ++index)
        m_pages.at(index)->setVisible(m_installed && !detail && index == m_page);
    for (int index = 0; index < m_pageTools.size(); ++index)
        m_pageTools.at(index)->setVisible(index == m_page);
    if (m_pageNav)
        m_pageNav->setVisible(m_installed);
    if (m_channelBar)
        m_channelBar->setVisible(m_installed && m_channelChips.size() > 1);
    for (QFrame *chip : std::as_const(m_channelChips)) {
        if (!chip)
            continue;
        const bool current = chip->property("channel").toString() == m_detailChannel &&
                             (detail || m_page == 0);
        if (chip->property("current").toBool() == current)
            continue;
        chip->setProperty("current", current);
        chip->style()->unpolish(chip);
        chip->style()->polish(chip);
    }
    m_detailScroll->setVisible(detail);
}

void MarketingFeature::renderChannelDetail()
{
    if (m_detailChannel.isEmpty() || !m_detailHead)
        return;
    const QString id = m_detailChannel;
    QJsonObject item;
    for (const QJsonValue &value : m_summaryData.value(QStringLiteral("channels")).toArray()) {
        if (text(value.toObject(), "channel") == id)
            item = value.toObject();
    }
    if (item.isEmpty() && m_summaryData.contains(QStringLiteral("channels"))) {
        closeChannel();
        return;
    }
    QJsonObject settings;
    for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray()) {
        if (text(value.toObject(), "id") == id)
            settings = value.toObject();
    }
    const DashboardColors colors = DashboardColors::from(this);
    const QJsonObject health = item.value(QStringLiteral("health")).toObject();
    const QString state = text(health, "state");
    const QString name = channelName(id, m_settings, m_channelCatalog);
    const QString category = channelCategory(id, m_channelCatalog);
    const QString link = normalizedChannelUrl(text(settings, "profileUrl"));
    const bool hasLink = link.startsWith(QLatin1String("https://"));
    const QString openLink = channelOpenUrl(settings);
    const bool reddit = id == QLatin1String("reddit");
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const QColor stateColor = state == QLatin1String("good") ? colors.good
        : state == QLatin1String("attention") || state == QLatin1String("stale") ? colors.warn
        : state == QLatin1String("setup") ? colors.bad : colors.muted;

    auto *headLayout = static_cast<QVBoxLayout *>(m_detailHead->layout());
    clearLayout(headLayout);
    m_detailRefresh.clear();
    auto *row = new QHBoxLayout;
    row->setSpacing(10);
    auto *back = linkButton(QString::fromUtf8("\xE2\x80\xB9"), QStringLiteral("Back to all channels"));
    back->setAccessibleName(QStringLiteral("All channels"));
    connect(back, &QPushButton::clicked, this, [this] { closeChannel(); });
    row->addWidget(back, 0, Qt::AlignVCenter);
    auto *glyph = new QLabel(initials(name));
    glyph->setFixedSize(36, 36);
    glyph->setAlignment(Qt::AlignCenter);
    glyph->setStyleSheet(QStringLiteral(
        "background: %1; color: #ffffff; border-radius: 9px; font-size: 13px; font-weight: 600;")
        .arg(brandColor(id, colors).name()));
    row->addWidget(glyph, 0, Qt::AlignVCenter);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(0);
    const QJsonObject feed = settings.value(QStringLiteral("feed")).toObject();
    const bool active = settings.value(QStringLiteral("enabled")).toBool(true);
    auto *title = new QLabel(QStringLiteral("<span style=\"color:%1; font-size:17px; font-weight:600\">%2</span>")
                                 .arg(colors.fg.name(), escaped(name)) +
                             (text(feed, "title").isEmpty() || text(feed, "title") == name ? QString()
                                  : QStringLiteral("<span style=\"color:%1; font-size:12px\">&nbsp;&nbsp;%2</span>")
                                        .arg(colors.muted.name(), escaped(text(feed, "title")))));
    title->setObjectName(QStringLiteral("marketingChannelTitle"));
    title->setTextFormat(Qt::RichText);
    titles->addWidget(title);
    QStringList facts;
    if (!category.isEmpty())
        facts << escaped(category);
    facts << QStringLiteral("<span style=\"color:%1\">● %2</span>")
                 .arg(stateColor.name(), escaped(healthLabel(state)));
    if (!active)
        facts << QStringLiteral("<span style=\"color:%1\">Paused</span>").arg(colors.warn.name());
    if (hasLink)
        facts << QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none\">%3</a>")
                     .arg(escaped(openLink), colors.primary.name(), escaped(QUrl(openLink).host() + QUrl(openLink).path()));
    if (feed.value(QStringLiteral("lastReadAt")).toDouble() > 0)
        facts << QStringLiteral("refreshed %1 · %2 posts read")
                     .arg(relativeLabel(qint64(number(feed, "lastReadAt"))))
                     .arg(feed.value(QStringLiteral("itemCount")).toInt());
    auto *sub = new QLabel(facts.join(dot));
    sub->setObjectName(QStringLiteral("marketingHint"));
    sub->setTextFormat(Qt::RichText);
    sub->setOpenExternalLinks(true);
    sub->setWordWrap(true);
    titles->addWidget(sub);
    row->addLayout(titles, 1);
    headLayout->addLayout(row);
    const QString bio = text(feed, "description");
    if (!bio.isEmpty()) {
        auto *about = new QLabel;
        about->setObjectName(QStringLiteral("marketingSmall"));
        about->setTextFormat(Qt::PlainText);
        about->setToolTip(bio);
        about->setWordWrap(true);
        about->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        about->setText(bio.size() > 220 ? bio.left(219).trimmed() + QChar(0x2026) : bio);
        headLayout->addWidget(about);
    }

    auto *actions = new QHBoxLayout;
    actions->setSpacing(6);
    const auto action = [actions](const QString &label, const QString &tip, bool primary = false) {
        auto *button = new QPushButton(label);
        button->setObjectName(primary ? QStringLiteral("primaryButton") : QStringLiteral("ghostButton"));
        if (!tip.isEmpty())
            button->setToolTip(tip);
        actions->addWidget(button);
        return button;
    };
    if (hasLink) {
        auto *open = action(QStringLiteral("Open ↗"), QStringLiteral("Open %1 in the browser").arg(name));
        connect(open, &QPushButton::clicked, this, [openLink] { QDesktopServices::openUrl(QUrl(openLink)); });
    }
    const bool refreshable = m_refreshableChannels.contains(id);
    const bool capturable = hasLink && (!refreshable || id == QLatin1String("google-search") || reddit);
    if (refreshable) {
        m_detailRefresh = action(QStringLiteral("Refresh"), QString());
        connect(m_detailRefresh, &QPushButton::clicked, this, [this, id] { refreshChannel(id); });
    }
    if (capturable) {
        auto *capture = action(QStringLiteral("Update"), QStringLiteral(
            "Update with the browser extension: opens %1 in your browser, where the ForkMesh extension reads its numbers.").arg(name));
        connect(capture, &QPushButton::clicked, this, [this, id] { captureWithExtension(id); });
    }
    auto *editChannel = action(QStringLiteral("Edit"), QStringLiteral("Link, display name and posting velocity in Manage channels"));
    connect(editChannel, &QPushButton::clicked, this, [this, id] { showChannelRow(id, true, true); });
    auto *log = action(QStringLiteral("Log numbers"), QStringLiteral("Log %1's numbers by hand").arg(name));
    connect(log, &QPushButton::clicked, this, [this, id] { addMetric(id); });
    auto *context = action(QStringLiteral("AI context"), QStringLiteral(
        "Copy %1's numbers, posts and a prompt to paste into an AI chat. It suggests how to grow the channel "
        "and checks its earlier advice against the numbers each time you copy again.").arg(name));
    connect(context, &QPushButton::clicked, this, [this, id] { copyChannelContext(id); });
    if (m_canManageSettings) {
        auto *remove = action(QStringLiteral("Delete"), QStringLiteral("Delete %1 from the dashboard").arg(name));
        connect(remove, &QPushButton::clicked, this, [this, id] { deleteChannel(id); });
    }
    auto *post = action(QStringLiteral("New post"), QStringLiteral("Draft a post for %1").arg(name), true);
    actions->removeWidget(post);
    row->addWidget(post, 0, Qt::AlignTop);
    connect(post, &QPushButton::clicked, this, [this, id] {
        editContent(QJsonObject{{QStringLiteral("channel"), id},
                                {QStringLiteral("status"), QStringLiteral("draft")}});
    });
    actions->addStretch(1);
    headLayout->addLayout(actions);
    updateRefreshButtons();

    const MetricSeries series = seriesByChannel(m_metrics);
    const QList<QJsonObject> history = series.value(id);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 range = m_rangeDays * kDay;
    const qint64 start = now - range;
    const Headline main = headline(id, item, health);
    KpiStrip::Kpi mainKpi;
    mainKpi.label = main.label;
    mainKpi.value = compactCount(qint64(main.value));
    mainKpi.tooltip = fullCount(main.value);
    const bool starsMain = main.key && QLatin1String(main.key) == QLatin1String("stars");
    const QJsonValue change = health.value(starsMain ? QStringLiteral("starsChange")
                                                     : QStringLiteral("followersChange"));
    if (main.key && (starsMain || QLatin1String(main.key) == QLatin1String("followers")) &&
        change.isDouble()) {
        const double delta = change.toDouble();
        mainKpi.delta = delta > 0 ? QStringLiteral("▲ %1").arg(compactCount(qint64(delta)))
                      : delta < 0 ? QStringLiteral("▼ %1").arg(compactCount(qint64(-delta)))
                                  : QStringLiteral("• 0");
        mainKpi.tone = delta > 0 ? 1 : delta < 0 ? -1 : 0;
        mainKpi.note = QStringLiteral("30 days");
    } else if (main.key) {
        const QJsonObject before = snapshotAt(history, start);
        if (!before.isEmpty()) {
            std::tie(mainKpi.delta, mainKpi.tone) = percentDelta(main.value, number(before, main.key));
            mainKpi.note = QStringLiteral("vs %1 days ago").arg(m_rangeDays);
        }
    }
    if (main.key) {
        for (int index = 0; index < 12; ++index)
            mainKpi.spark << number(snapshotAt(history, start + range * index / 11), main.key);
    }

    KpiStrip::Kpi reachKpi;
    reachKpi.label = reddit ? QStringLiteral("Weekly visitors") : QStringLiteral("Reach");
    reachKpi.value = number(item, "reach") > 0 ? compactCount(qint64(number(item, "reach"))) : kDash;
    reachKpi.tooltip = fullCount(number(item, "reach"));
    if (number(item, "engagements") > 0)
        reachKpi.note = QStringLiteral("%1 %2").arg(compactCount(qint64(number(item, "engagements"))),
                                                    reddit ? QStringLiteral("weekly contributions")
                                                           : QStringLiteral("engaged"));

    KpiStrip::Kpi engagement;
    engagement.label = QStringLiteral("Engagement");
    engagement.value = health.value(QStringLiteral("engagementRate")).isDouble()
        ? QString::number(number(health, "engagementRate"), 'f', 1) + QLatin1Char('%') : kDash;
    engagement.note = engagementBaseLabel(health);
    engagement.tooltip = engagementFormula(health);
    if (id == QLatin1String("email")) {
        reachKpi = {};
        reachKpi.label = QStringLiteral("Closed deals");
        reachKpi.value = fullCount(number(item, "closedDeals"));
        reachKpi.tooltip = QStringLiteral("CRM contacts at a won stage: won, customer, committed, signed, paid or active");
        if (number(item, "closedValue") > 0)
            reachKpi.note = QStringLiteral("worth %1").arg(compactCount(qint64(number(item, "closedValue"))));
        engagement = {};
        engagement.label = QStringLiteral("Replies · 30D");
        engagement.value = fullCount(number(item, "replies"));
        engagement.note = QStringLiteral("linked in CRM");
    }
    if (id == QLatin1String("github")) {
        reachKpi = {};
        reachKpi.label = starsMain ? QStringLiteral("Followers") : QStringLiteral("Stars");
        reachKpi.value = fullCount(number(item, starsMain ? "followers" : "stars"));
        reachKpi.tooltip = starsMain ? QStringLiteral("People following the GitHub account")
                                     : QStringLiteral("Stars on the linked repository, or summed over the account's own repositories");
        engagement = {};
        engagement.label = QStringLiteral("Forks");
        engagement.value = fullCount(number(item, "forks"));
        engagement.tooltip = QStringLiteral("Forks of the linked repository, or summed over the account's own repositories");
    }

    if (id == QLatin1String("x") || (number(item, "following") > 0 && number(item, "reach") <= 0)) {
        reachKpi = {};
        reachKpi.label = QStringLiteral("Following");
        reachKpi.value = missing(item, "following") ? kDash : compactCount(qint64(number(item, "following")));
        reachKpi.tooltip = QStringLiteral("Accounts %1 follows").arg(name);
        const QJsonObject before = snapshotAt(history, start);
        if (!missing(item, "following") && !before.isEmpty() && !missing(before, "following")) {
            const qint64 delta = qint64(number(item, "following") - number(before, "following"));
            reachKpi.delta = delta > 0 ? QStringLiteral("▲ %1").arg(compactCount(delta))
                           : delta < 0 ? QStringLiteral("▼ %1").arg(compactCount(-delta))
                                       : QStringLiteral("• 0");
            reachKpi.note = QStringLiteral("vs %1 days ago").arg(m_rangeDays);
        }
    }

    const int posts = int(number(health, "postsLast30d"));
    const int cadence = qMax(1, int(number(health, "cadenceDays")));
    KpiStrip::Kpi postsKpi;
    postsKpi.label = QStringLiteral("Posts · 30D");
    postsKpi.value = fullCount(posts);
    postsKpi.note = QStringLiteral("aim: every %1 day%2").arg(cadence).arg(cadence == 1 ? QString() : QStringLiteral("s"));

    KpiStrip::Kpi lastPost;
    lastPost.label = QStringLiteral("Last post");
    const qint64 lastPostAt = qint64(number(health, "lastPostAt"));
    lastPost.value = lastPostAt > 0 ? relativeLabel(lastPostAt) : kDash;
    lastPost.tooltip = lastPostAt > 0 ? timestamp(lastPostAt) : QString();
    lastPost.note = lastPostAt > 0 ? shortDate(lastPostAt) : QStringLiteral("nothing published yet");

    KpiStrip::Kpi numbers;
    numbers.label = QStringLiteral("Numbers");
    const QJsonValue age = health.value(QStringLiteral("ageDays"));
    numbers.value = age.isDouble() ? agoText(qint64(age.toDouble())) : QStringLiteral("never");
    numbers.note = age.isDouble() ? sourceLabel(text(health, "source")) : QStringLiteral("Refresh or log them");
    QHash<QString, bool> checks;
    for (const QJsonValue &value : health.value(QStringLiteral("checks")).toArray())
        checks.insert(text(value.toObject(), "id"), value.toObject().value(QStringLiteral("ok")).toBool());
    numbers.tone = checks.value(QStringLiteral("fresh"), true) ? 0 : -1;
    if (numbers.tone < 0)
        numbers.delta = QStringLiteral("stale");
    int scheduled = 0;
    int drafts = 0;
    int pipeline = 0;
    for (const QJsonValue &value : std::as_const(m_content)) {
        const QJsonObject entry = value.toObject();
        if (text(entry, "channel") != id || text(entry, "status") == QLatin1String("archived"))
            continue;
        ++pipeline;
        scheduled += text(entry, "status") == QLatin1String("scheduled") ? 1 : 0;
        drafts += text(entry, "status") == QLatin1String("draft") || text(entry, "status") == QLatin1String("review") ? 1 : 0;
    }
    KpiStrip::Kpi pipelineKpi;
    pipelineKpi.label = QStringLiteral("Pipeline");
    pipelineKpi.value = fullCount(pipeline);
    pipelineKpi.note = QStringLiteral("%1 scheduled · %2 draft%3").arg(scheduled).arg(drafts)
                           .arg(drafts == 1 ? QString() : QStringLiteral("s"));
    pipelineKpi.tooltip = QStringLiteral("Content items for %1, archived ones left out").arg(name);

    KpiStrip::Kpi perPost;
    perPost.label = QStringLiteral("Per post");
    const int read = settings.value(QStringLiteral("feed")).toObject().value(QStringLiteral("itemCount")).toInt();
    perPost.value = number(item, "engagements") > 0 && read > 0
        ? QString::number(number(item, "engagements") / read, 'f', number(item, "engagements") / read < 10 ? 1 : 0)
        : kDash;
    perPost.note = read > 0 ? QStringLiteral("over the last %1 posts").arg(read) : QStringLiteral("no posts read yet");
    perPost.tooltip = QStringLiteral("Likes, replies and reposts per post read at the last refresh");
    m_detailKpis->setKpis({mainKpi, reachKpi, engagement, perPost, postsKpi, lastPost, pipelineKpi, numbers});

    const auto note = [](QVBoxLayout *box, const QString &value, const char *object = "marketingNote") {
        auto *label = new QLabel(value);
        label->setObjectName(QLatin1String(object));
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        box->addWidget(label);
        return label;
    };

    clearLayout(m_detailSteps);
    QList<forkmesh::market::NativePackageRailStep> steps;
    for (const forkmesh::market::NativePackageRailTile &tile : railTiles()) {
        if (tile.id == id)
            steps = tile.steps;
    }
    for (const forkmesh::market::NativePackageRailStep &step : std::as_const(steps)) {
        if (step.id == QLatin1String("open"))
            continue;
        auto *label = note(m_detailSteps, QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none\">%3 %4</a>")
                                              .arg(escaped(step.id), colors.primary.name(),
                                                   QString::fromUtf8("\xE2\x86\x92"), escaped(step.text)));
        label->setCursor(Qt::PointingHandCursor);
        const QString stepId = step.id;
        connect(label, &QLabel::linkActivated, this, [this, id, stepId] { activateRailStep(id, stepId); });
    }
    if (m_detailSteps->count() == 0)
        note(m_detailSteps, QStringLiteral("Nothing to do right now."), "marketingHint");
    m_detailSteps->addStretch(1);

    clearLayout(m_detailHealth);
    const auto mark = [&colors](bool ok) {
        return QStringLiteral("<span style=\"color:%1\">%2</span>")
            .arg((ok ? colors.good : colors.bad).name(),
                 ok ? QString::fromUtf8("\xE2\x9C\x93") : QString::fromUtf8("\xE2\x9C\x97"));
    };
    note(m_detailHealth, QStringLiteral("<span style=\"color:%1; font-weight:600\">%2</span>%3%4 of %5 checks pass")
                             .arg(stateColor.name(), escaped(healthLabel(state)), dot)
                             .arg(int(std::count(checks.cbegin(), checks.cend(), true)))
                             .arg(checks.size()));
    for (const QJsonValue &value : health.value(QStringLiteral("checks")).toArray()) {
        const QJsonObject check = value.toObject();
        note(m_detailHealth, mark(check.value(QStringLiteral("ok")).toBool()) + QStringLiteral(" ") +
                                 escaped(text(check, "label")));
    }
    const QJsonArray guide = health.value(QStringLiteral("guide")).toArray();
    if (!guide.isEmpty()) {
        note(m_detailHealth, QStringLiteral("Get %1's numbers").arg(escaped(name)), "marketingHint");
        for (int index = 0; index < guide.size(); ++index) {
            const QJsonObject guideStep = guide.at(index).toObject();
            note(m_detailHealth, mark(guideStep.value(QStringLiteral("done")).toBool()) +
                                     QStringLiteral(" %1. %2").arg(index + 1).arg(escaped(text(guideStep, "label"))));
        }
    }
    if (!text(feed, "lastError").isEmpty())
        note(m_detailHealth, escaped(QStringLiteral("Last refresh failed: ") +
                                     refreshErrorText(id, text(feed, "lastError"), text(feed, "lastError"))),
             "marketingBad");
    if (settings.value(QStringLiteral("hasAccessToken")).toBool())
        note(m_detailHealth, mark(true) + QStringLiteral(" API access token saved"));
    m_detailHealth->addStretch(1);

    clearLayout(m_detailPlaybook);
    const QJsonObject playbook = m_playbooks.value(category).toObject();
    if (playbook.isEmpty()) {
        note(m_detailPlaybook, QStringLiteral("No playbook for this channel's category yet."), "marketingHint");
    } else {
        note(m_detailPlaybook, QStringLiteral("<b>Goal:</b> ") + escaped(text(playbook, "goal")));
        QStringList kpis;
        for (const QJsonValue &kpi : playbook.value(QStringLiteral("kpis")).toArray())
            kpis << kpi.toString();
        if (!kpis.isEmpty())
            note(m_detailPlaybook, QStringLiteral("<b>Track:</b> ") + escaped(kpis.join(QStringLiteral(", "))));
        for (const QJsonValue &tip : playbook.value(QStringLiteral("tips")).toArray())
            note(m_detailPlaybook, QString::fromUtf8("\xE2\x80\xA2 ") + escaped(tip.toString()));
    }
    m_detailPlaybook->addStretch(1);

    QList<QJsonObject> content;
    for (const QJsonValue &value : std::as_const(m_content)) {
        if (text(value.toObject(), "channel") == id)
            content.append(value.toObject());
    }
    const auto when = [](const QJsonObject &entry) {
        for (const char *key : {"publishedAt", "scheduledAt", "updatedAt"}) {
            if (number(entry, key) > 0)
                return qint64(number(entry, key));
        }
        return qint64(0);
    };
    std::stable_sort(content.begin(), content.end(), [&when](const QJsonObject &a, const QJsonObject &b) {
        return when(a) > when(b);
    });
    QHash<QString, QJsonObject> pageViews;
    for (const QJsonValue &value : m_traffic.value(QStringLiteral("pages")).toArray()) {
        const QJsonObject page = value.toObject();
        pageViews.insert(text(page, "host") + text(page, "path"), page);
    }
    const QJsonObject channelTraffic = channelViews(id);
    double contentViews = 0;
    m_detailContent->setRowCount(0);
    for (const QJsonObject &entry : std::as_const(content)) {
        const int contentRow = m_detailContent->rowCount();
        m_detailContent->insertRow(contentRow);
        const QJsonObject page = pageViews.value(pageKey(text(entry, "url")));
        contentViews += number(page, "views");
        const QStringList values{text(entry, "title"), channelName(text(entry, "status")),
                                 when(entry) > 0 ? timestamp(when(entry)) : kDash,
                                 page.isEmpty() ? kDash : fullCount(number(page, "views")),
                                 page.isEmpty() ? kDash : fullCount(number(page, "visitors")),
                                 text(entry, "campaign")};
        for (int column = 0; column < values.size(); ++column) {
            if (column == 3 || column == 4) {
                QTableWidgetItem *cell = numberItem(values.at(column));
                if (!page.isEmpty())
                    cell->setToolTip(QStringLiteral("%1 views from %2 visitors in the last %3 days · last view %4")
                                         .arg(fullCount(number(page, "views")), fullCount(number(page, "visitors")))
                                         .arg(m_rangeDays).arg(timestamp(qint64(number(page, "lastAt")))));
                m_detailContent->setItem(contentRow, column, cell);
                continue;
            }
            auto *cell = new QTableWidgetItem(values.at(column));
            if (column == 0) {
                cell->setData(Qt::UserRole, text(entry, "id"));
                cell->setToolTip(text(entry, "body").left(400));
            }
            if (column == 1 && text(entry, "status") == QLatin1String("published"))
                cell->setForeground(colors.good);
            if (column == 1 && text(entry, "status") == QLatin1String("review"))
                cell->setForeground(colors.warn);
            m_detailContent->setItem(contentRow, column, cell);
        }
    }
    m_detailContent->setColumnHidden(3, channelTraffic.isEmpty());
    m_detailContent->setColumnHidden(4, channelTraffic.isEmpty());
    m_detailContent->setVisible(!content.isEmpty());
    fitTable(m_detailContent, 10);

    m_detailAds->setRowCount(0);
    for (const QJsonValue &value : std::as_const(m_ads)) {
        const QJsonObject ad = value.toObject();
        if (text(ad, "channel") != id)
            continue;
        const int adRow = m_detailAds->rowCount();
        m_detailAds->insertRow(adRow);
        auto *nameCell = new QTableWidgetItem(text(ad, "name"));
        nameCell->setData(Qt::UserRole, text(ad, "id"));
        m_detailAds->setItem(adRow, 0, nameCell);
        m_detailAds->setItem(adRow, 1, new QTableWidgetItem(adLabel(text(ad, "status"))));
        m_detailAds->setItem(adRow, 2, numberItem(money(ad.value(QStringLiteral("spendCents")), text(ad, "currency"))));
    }
    m_detailAdsPanel->hint()->setText(m_detailAds->rowCount()
        ? QStringLiteral("%1 ad%2").arg(m_detailAds->rowCount()).arg(m_detailAds->rowCount() == 1 ? QString() : QStringLiteral("s"))
        : QStringLiteral("No ads on %1").arg(name));
    m_detailAds->setVisible(m_detailAds->rowCount() > 0);
    fitTable(m_detailAds, 8);

    const int postRows = fillPostedLinks(m_detailPosts, id);
    m_detailPostsNote->setText(postRows
        ? QStringLiteral("Posted links")
        : id == QLatin1String("x")
            ? QStringLiteral("Add a link, or scroll your X profile and press Sync now in the ForkMesh extension")
            : QStringLiteral("Add a link to a post on %1 to track its stats").arg(name));
    m_detailContentPanel->hint()->setText(
        (content.isEmpty() ? QStringLiteral("Nothing for %1 yet").arg(name)
                           : QStringLiteral("%1 item%2").arg(content.size())
                                 .arg(content.size() == 1 ? QString() : QStringLiteral("s"))) +
        (postRows ? QStringLiteral(" · %1 link%2").arg(postRows).arg(postRows == 1 ? QString() : QStringLiteral("s"))
                  : QString()) +
        (contentViews > 0 ? QStringLiteral(" · %1 views in %2D").arg(compactCount(qint64(contentViews))).arg(m_rangeDays)
                          : QString()));
    m_detailPosts->setVisible(postRows > 0);
    fitTable(m_detailPosts, 12);

    m_detailSnapshots->setRowCount(0);
    static constexpr const char *snapshotKeys[] = {"followers", "following", "posts", "reach", "engagements",
                                                   "views", "visits", "stars", "forks"};
    for (int index = 1; index < int(std::size(snapshotKeys)); ++index) {
        const char *key = snapshotKeys[index];
        m_detailSnapshots->setColumnHidden(index + 1, std::none_of(history.cbegin(), history.cend(),
            [key](const QJsonObject &snapshot) { return number(snapshot, key) > 0; }));
    }
    for (auto it = history.crbegin(); it != history.crend(); ++it) {
        const QJsonObject &snapshot = *it;
        const int snapshotRow = m_detailSnapshots->rowCount();
        m_detailSnapshots->insertRow(snapshotRow);
        m_detailSnapshots->setItem(snapshotRow, 0, new QTableWidgetItem(timestamp(qint64(number(snapshot, "observedAt")))));
        int column = 1;
        for (const char *key : snapshotKeys)
            m_detailSnapshots->setItem(snapshotRow, column++, numberItem(
                missing(snapshot, key) ? kDash : fullCount(number(snapshot, key))));
        m_detailSnapshots->setItem(snapshotRow, 10, new QTableWidgetItem(sourceLabel(text(snapshot, "source"))));
        m_detailSnapshots->setItem(snapshotRow, 11, new QTableWidgetItem(text(snapshot, "note")));
    }
    m_detailSnapshotsPanel->hint()->setText(history.isEmpty()
        ? QStringLiteral("No numbers recorded yet. Refresh %1 or log its numbers.").arg(name)
        : QStringLiteral("%1 snapshot%2").arg(history.size()).arg(history.size() == 1 ? QString() : QStringLiteral("s")));
    m_detailSnapshots->setVisible(!history.isEmpty());
    fitTable(m_detailSnapshots, 8);

    const bool mastodon = id == QLatin1String("mastodon");
    m_detailPeoplePanel->setVisible(mastodon);
    m_detailSearchesPanel->setVisible(mastodon);
    if (mastodon && !m_mastodonLoading && m_mastodonOrg != m_organization->currentData().toString())
        loadMastodon();
    else if (mastodon)
        renderMastodon();

    const bool email = id == QLatin1String("email");
    m_detailEmailsPanel->setVisible(email);
    if (email && !m_emailSuggestionsLoading &&
        m_emailSuggestionsOrg != m_organization->currentData().toString())
        loadEmailSuggestions();
    else if (email)
        renderEmailSuggestions();

    const bool discord = id == QLatin1String("discord");
    m_detailDiscordPanel->setVisible(discord);
    m_detailDiscordChartPanel->setVisible(discord);
    if (discord) {
        renderDiscordPosts();
        loadDiscordPosts();
    }

    const bool ready = id == QLatin1String("x");
    m_detailReadyPanel->setVisible(ready);
    if (ready && !m_readyPostLoading &&
        (m_readyPostOrg != m_organization->currentData().toString() ||
         (!m_readyPostOverview.isEmpty() &&
          text(m_readyPostOverview.value(QStringLiteral("post")).toObject(), "id") != m_readyPostSeen)))
        loadReadyPost();
    else if (ready)
        renderReadyPost();

    m_detailRedditBox->setVisible(reddit);
    if (reddit)
        renderReddit();
    renderChannelDesk(id);
    renderChannelAgents(id);
    renderChannelChart();
}

void MarketingFeature::renderChannelAgents(const QString &channel)
{
    clearLayout(m_detailAgent);
    const QList<QJsonObject> agents = m_specialAgents ? agentsOnChannel(m_specialAgents(), channel)
                                                      : QList<QJsonObject>();
    m_detailAgentPanel->setVisible(!agents.isEmpty());
    if (agents.isEmpty())
        return;
    m_detailAgentPanel->titleLabel()->setText(agents.size() == 1 ? QStringLiteral("Special agent")
                                                                 : QStringLiteral("Special agents"));
    m_detailAgentPanel->hint()->setText(
        QStringLiteral("Works %1 for you").arg(channelName(channel, m_settings, m_channelCatalog)));
    const DashboardColors colors = DashboardColors::from(this);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    for (const QJsonObject &agent : std::as_const(agents)) {
        const QString agentId = text(agent, "id");
        const QString name = text(agent, "name");
        const QString status = text(agent, "status");
        const bool working = status == QLatin1String("Working") || status == QLatin1String("Queued") ||
                             status == QLatin1String("Waiting");
        const bool enabled = agent.value(QStringLiteral("enabled")).toBool(true);
        const QJsonObject schedule = agent.value(QStringLiteral("schedule")).toObject();

        auto *row = new QHBoxLayout;
        row->setSpacing(10);
        row->addWidget(agentFace(agent, 32, colors, this), 0, Qt::AlignTop);

        auto *about = new QVBoxLayout;
        about->setSpacing(1);
        auto *title = new QLabel(QStringLiteral("<b>%1</b>%2%3").arg(escaped(name), dot, escaped(text(agent, "specialty"))));
        title->setTextFormat(Qt::RichText);
        title->setWordWrap(true);
        about->addWidget(title);
        const QColor statusColor = working ? colors.good : status == QLatin1String("Paused") ? colors.muted
            : status == QLatin1String("Scheduled") ? colors.primary : colors.muted;
        QStringList facts{QStringLiteral("<span style=\"color:%1; font-weight:600\">%2</span>")
                              .arg(statusColor.name(), escaped(status.isEmpty() ? QStringLiteral("Idle") : status))};
        if (!text(schedule, "human").isEmpty())
            facts << escaped(text(schedule, "human"));
        if (!text(agent, "lastActive").isEmpty())
            facts << escaped(QStringLiteral("Last active ") + text(agent, "lastActive"));
        auto *state = new QLabel(facts.join(dot));
        state->setObjectName(QStringLiteral("marketingHint"));
        state->setTextFormat(Qt::RichText);
        state->setWordWrap(true);
        about->addWidget(state);
        if (!text(agent, "focus").isEmpty()) {
            auto *focus = new QLabel(escaped(text(agent, "focus")));
            focus->setObjectName(QStringLiteral("marketingNote"));
            focus->setTextFormat(Qt::RichText);
            focus->setWordWrap(true);
            about->addWidget(focus);
        }
        row->addLayout(about, 1);

        auto *open = new QPushButton(QStringLiteral("Open"));
        open->setObjectName(QStringLiteral("ghostButton"));
        open->setToolTip(QStringLiteral("Open %1's page to read its runs and talk to it").arg(name));
        connect(open, &QPushButton::clicked, this, [this, agentId] { runSpecialAgentAction(agentId, QStringLiteral("open")); });
        row->addWidget(open, 0, Qt::AlignTop);
        auto *run = new QPushButton(working ? QStringLiteral("Running") : QStringLiteral("Run now"));
        run->setObjectName(QStringLiteral("ghostButton"));
        run->setEnabled(!working && enabled);
        run->setToolTip(enabled ? QStringLiteral("Start a run of %1 now").arg(name)
                                : QStringLiteral("%1 is paused. Turn it on from the Special agents page.").arg(name));
        connect(run, &QPushButton::clicked, this, [this, agentId] { runSpecialAgentAction(agentId, QStringLiteral("run")); });
        row->addWidget(run, 0, Qt::AlignTop);
        m_detailAgent->addLayout(row);

        if (!text(agent, "workflow").isEmpty())
            continue;
        auto *ask = new QHBoxLayout;
        ask->setSpacing(6);
        ask->setContentsMargins(42, 0, 0, 4);
        auto *message = new QLineEdit;
        message->setPlaceholderText(QStringLiteral("Message %1 about %2…")
                                        .arg(name, channelName(channel, m_settings, m_channelCatalog)));
        message->setAccessibleName(QStringLiteral("Message for %1").arg(name));
        message->setClearButtonEnabled(true);
        auto *send = new QPushButton(QStringLiteral("Send"));
        send->setToolTip(working ? QStringLiteral("Steer %1's run with this message").arg(name)
                                 : QStringLiteral("Start a run of %1 with this message as its task").arg(name));
        send->setEnabled(false);
        connect(message, &QLineEdit::textChanged, send, [send](const QString &value) {
            send->setEnabled(!value.trimmed().isEmpty());
        });
        const auto submit = [this, agentId, message] {
            const QString value = message->text().trimmed();
            if (value.isEmpty())
                return;
            message->clear();
            runSpecialAgentAction(agentId, QStringLiteral("prompt"), value);
        };
        connect(message, &QLineEdit::returnPressed, this, submit);
        connect(send, &QPushButton::clicked, this, submit);
        ask->addWidget(message, 1);
        ask->addWidget(send);
        m_detailAgent->addLayout(ask);
    }
}

void MarketingFeature::runSpecialAgentAction(const QString &agentId, const QString &action, const QString &text)
{
    if (!m_specialAgentAction)
        return;
    QPointer<MarketingFeature> guard(this);
    m_specialAgentAction(agentId, action, text, [guard, action](bool ok, const QString &message) {
        if (!guard)
            return;
        guard->setStatus(message, !ok);
        if (ok && action != QLatin1String("open") && !guard->m_detailChannel.isEmpty())
            guard->renderChannelAgents(guard->m_detailChannel);
        if (ok && action != QLatin1String("open") && guard->m_detailChannel == QLatin1String("x"))
            guard->renderReadyPost();
    });
}

void MarketingFeature::loadMastodon()
{
    const QString organization = m_organization->currentData().toString();
    if (!m_installed || organization.isEmpty() || m_mastodonLoading)
        return;
    QStringList topics;
    for (const QJsonValue &value : std::as_const(m_keywords)) {
        const QString term = text(value.toObject(), "term");
        if (!term.isEmpty() && !term.contains(QLatin1Char(' ')) && topics.size() < 4)
            topics << term;
    }
    QHash<QString, int> tags;
    for (const QJsonValue &value : std::as_const(m_content)) {
        if (text(value.toObject(), "channel") != QLatin1String("mastodon"))
            continue;
        for (const QJsonValue &tag : value.toObject().value(QStringLiteral("tags")).toArray())
            ++tags[tag.toString()];
    }
    QStringList popular = tags.keys();
    std::sort(popular.begin(), popular.end(), [&tags](const QString &a, const QString &b) {
        return tags.value(a) != tags.value(b) ? tags.value(a) > tags.value(b) : a < b;
    });
    topics = popular.mid(0, 4) + topics;
    m_mastodonLoading = true;
    m_mastodonError.clear();
    if (m_mastodonOrg != organization) {
        m_mastodon = {};
        m_mastodonFollowed.clear();
        m_mastodonOpened.clear();
    }
    m_mastodonOrg = organization;
    renderMastodon();
    request(QByteArrayLiteral("GET"),
            organizationPath(QStringLiteral("/mastodon?topics=")) +
                QString::fromUtf8(QUrl::toPercentEncoding(topics.join(QLatin1Char(',')))),
            {}, [this, organization](bool ok, const QJsonObject &payload, const QString &error) {
        m_mastodonLoading = false;
        if (organization != m_organization->currentData().toString())
            return;
        if (ok)
            m_mastodon = payload.value(QStringLiteral("mastodon")).toObject();
        else
            m_mastodonError = refreshErrorText(QStringLiteral("mastodon"), text(payload, "error"), error);
        if (m_detailChannel == QLatin1String("mastodon"))
            renderMastodon();
    });
}

void MarketingFeature::renderMastodon()
{
    if (!m_detailPeople || !m_detailSearches)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const auto note = [](QVBoxLayout *box, const QString &value, const char *object = "marketingNote") {
        auto *label = new QLabel(value);
        label->setObjectName(QLatin1String(object));
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setOpenExternalLinks(true);
        box->addWidget(label);
        return label;
    };
    const auto anchor = [&colors](const QString &url, const QString &label) {
        return QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none\">%3</a>")
            .arg(escaped(url), colors.primary.name(), escaped(label));
    };
    const QString server = text(m_mastodon, "server");
    const bool canFollow = m_mastodon.value(QStringLiteral("canFollow")).toBool();

    clearLayout(m_detailPeople);
    const QJsonArray people = m_mastodon.value(QStringLiteral("suggestions")).toArray();
    if (m_mastodonLoading && people.isEmpty())
        note(m_detailPeople, QStringLiteral("Looking for people posting about your topics…"), "marketingHint");
    else if (!m_mastodonError.isEmpty())
        note(m_detailPeople, escaped(QStringLiteral("Could not load suggestions: ") + m_mastodonError), "marketingBad");
    else if (people.isEmpty() && !m_mastodon.isEmpty())
        note(m_detailPeople, QStringLiteral("No new people found on %1 right now. Try the searches alongside.")
                                 .arg(escaped(server)), "marketingHint");
    if (!m_mastodon.isEmpty() && !canFollow) {
        auto *setup = note(m_detailPeople, m_mastodon.value(QStringLiteral("hasToken")).toBool()
            ? QStringLiteral("Follow opens the profile on %1; only owners and admins follow from here.").arg(escaped(server))
            : QStringLiteral("Follow opens the profile on %1. <a href=\"token\" style=\"color:%2; text-decoration:none\">Add an access token</a> to follow in one click.")
                  .arg(escaped(server), colors.primary.name()),
            "marketingHint");
        setup->setOpenExternalLinks(false);
        connect(setup, &QLabel::linkActivated, this, [this] { promptChannelToken(QStringLiteral("mastodon")); });
    }
    QList<QWidget *> cards;
    for (const QJsonValue &value : people) {
        const QJsonObject person = value.toObject();
        const QString personId = text(person, "id");
        if (m_mastodonFollowed.contains(personId))
            continue;
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("marketingFact"));
        auto *row = new QHBoxLayout(card);
        row->setContentsMargins(8, 6, 8, 6);
        row->setSpacing(8);
        auto *glyph = new QLabel(initials(text(person, "name")));
        glyph->setFixedSize(28, 28);
        glyph->setAlignment(Qt::AlignCenter);
        glyph->setStyleSheet(QStringLiteral(
            "background: %1; color: #ffffff; border-radius: 14px; font-size: 11px; font-weight: 600;")
            .arg(brandColor(QStringLiteral("mastodon"), colors).name()));
        row->addWidget(glyph, 0, Qt::AlignTop);
        QStringList facts{QStringLiteral("%1 followers").arg(compactCount(qint64(number(person, "followers")))),
                          QStringLiteral("%1 posts").arg(compactCount(qint64(number(person, "posts"))))};
        if (number(person, "lastPostAt") > 0)
            facts << QStringLiteral("active %1").arg(relativeLabel(qint64(number(person, "lastPostAt"))));
        QStringList reasons;
        for (const QJsonValue &reason : person.value(QStringLiteral("reasons")).toArray())
            reasons << reason.toString();
        if (person.value(QStringLiteral("suggested")).toBool())
            reasons << QStringLiteral("suggested by %1").arg(server);
        if (person.value(QStringLiteral("locked")).toBool())
            facts << QStringLiteral("approves followers");
        const QString profile = text(person, "url").isEmpty() ? text(person, "followUrl") : text(person, "url");
        auto *details = new QLabel(
            QStringLiteral("<b>%1</b> <span style=\"color:%2\">%3</span><br>"
                           "<span style=\"color:%2; font-size:11px\">%4</span>")
                .arg(anchor(profile, text(person, "name")), colors.muted.name(),
                     escaped(QLatin1Char('@') + text(person, "handle")),
                     escaped((facts + (reasons.isEmpty() ? QStringList()
                                                          : QStringList{reasons.join(QStringLiteral(", "))})).join(dot))));
        details->setObjectName(QStringLiteral("marketingNote"));
        details->setTextFormat(Qt::RichText);
        details->setOpenExternalLinks(true);
        details->setWordWrap(true);
        details->setToolTip(text(person, "note"));
        details->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row->addWidget(details, 1);
        auto *follow = new QPushButton;
        follow->setObjectName(QStringLiteral("primaryButton"));
        if (m_mastodonPending.contains(personId)) {
            follow->setText(QStringLiteral("Following…"));
            follow->setEnabled(false);
        } else if (m_mastodonOpened.contains(personId)) {
            follow->setText(QStringLiteral("Opened ↗"));
            follow->setToolTip(QStringLiteral("Opened @%1 on %2").arg(text(person, "handle"), server));
            follow->setEnabled(false);
        } else {
            follow->setCursor(Qt::PointingHandCursor);
            follow->setText(canFollow ? QStringLiteral("Follow") : QStringLiteral("Follow ↗"));
            follow->setToolTip(canFollow
                ? QStringLiteral("Follow @%1 from @%2").arg(text(person, "handle"), text(m_mastodon, "account"))
                : QStringLiteral("Open @%1 on %2 to follow").arg(text(person, "handle"), server));
            connect(follow, &QPushButton::clicked, this, [this, person] { followMastodon(person); });
        }
        row->addWidget(follow, 0, Qt::AlignVCenter);
        cards << card;
    }
    if (cards.isEmpty() && !people.isEmpty())
        note(m_detailPeople, QStringLiteral("You follow everyone found on %1 so far. Find more looks again.")
                                 .arg(escaped(server)), "marketingHint");
    if (!cards.isEmpty()) {
        auto *list = clearWidget();
        auto *listLayout = new QVBoxLayout(list);
        listLayout->setContentsMargins(0, 0, 4, 0);
        listLayout->setSpacing(6);
        for (QWidget *card : std::as_const(cards))
            listLayout->addWidget(card);
        listLayout->addStretch(1);
        auto *scroller = new QScrollArea;
        scroller->setObjectName(QStringLiteral("marketingClear"));
        scroller->setAccessibleName(QStringLiteral("People to follow"));
        scroller->setFrameShape(QFrame::NoFrame);
        scroller->setWidgetResizable(true);
        scroller->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroller->viewport()->setAutoFillBackground(false);
        scroller->setMinimumHeight(260);
        scroller->setWidget(list);
        m_detailPeople->addWidget(scroller, 1);
    } else {
        m_detailPeople->addStretch(1);
    }
    m_detailPeoplePanel->hint()->setText(m_mastodonLoading ? QStringLiteral("Looking…")
        : cards.isEmpty() ? QString()
        : QStringLiteral("%1 %2 on %3").arg(cards.size())
              .arg(cards.size() == 1 ? QStringLiteral("person") : QStringLiteral("people"), server));

    clearLayout(m_detailSearches);
    const QJsonArray searches = m_mastodon.value(QStringLiteral("searches")).toArray();
    if (searches.isEmpty() && m_mastodonLoading)
        note(m_detailSearches, QStringLiteral("Loading…"), "marketingHint");
    const auto linkLabel = [](const QString &html, const QString &tip) -> QWidget * {
        auto *label = new QLabel(html);
        label->setObjectName(QStringLiteral("marketingNote"));
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setOpenExternalLinks(true);
        label->setToolTip(tip);
        return label;
    };
    const auto linkGrid = [this](const QList<QWidget *> &links) {
        auto *grid = new TileGrid(160, 6);
        grid->setTiles(links);
        m_detailSearches->addWidget(grid);
    };
    QList<QWidget *> links;
    for (const QJsonValue &value : searches) {
        const QJsonObject search = value.toObject();
        links << linkLabel(anchor(text(search, "tagUrl"), QLatin1Char('#') + text(search, "topic")) + dot +
                               anchor(text(search, "peopleUrl"), QStringLiteral("people")),
                           QStringLiteral("Posts tagged #%1, or people who post about it").arg(text(search, "topic")));
    }
    if (!links.isEmpty())
        linkGrid(links);
    const QJsonArray places = m_mastodon.value(QStringLiteral("places")).toArray();
    if (!places.isEmpty())
        note(m_detailSearches, QStringLiteral("Browse"), "marketingHint");
    links.clear();
    for (const QJsonValue &value : places) {
        const QJsonObject place = value.toObject();
        links << linkLabel(anchor(text(place, "url"), text(place, "label") + QStringLiteral(" ↗")), text(place, "note"));
    }
    if (!links.isEmpty())
        linkGrid(links);
    m_detailSearches->addStretch(1);
    m_detailSearchesPanel->hint()->setText(searches.isEmpty() ? QString()
        : QStringLiteral("%1 topic%2").arg(searches.size()).arg(searches.size() == 1 ? QString() : QStringLiteral("s")));
}

void MarketingFeature::followMastodon(const QJsonObject &person)
{
    const QString personId = text(person, "id");
    if (!m_mastodon.value(QStringLiteral("canFollow")).toBool()) {
        QDesktopServices::openUrl(QUrl(text(person, "followUrl")));
        if (!personId.isEmpty()) {
            m_mastodonOpened.insert(personId);
            renderMastodon();
        }
        return;
    }
    if (personId.isEmpty() || m_mastodonPending.contains(personId))
        return;
    m_mastodonPending.insert(personId);
    renderMastodon();
    const QString handle = text(person, "handle");
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/mastodon")),
            QJsonObject{{QStringLiteral("follow"), personId}},
            [this, personId, handle](bool ok, const QJsonObject &payload, const QString &error) {
        m_mastodonPending.remove(personId);
        if (ok) {
            m_mastodonFollowed.insert(personId, text(payload, "relationship"));
            setStatus(text(payload, "relationship") == QLatin1String("requested")
                ? QStringLiteral("Follow request sent to @%1.").arg(handle)
                : QStringLiteral("Now following @%1.").arg(handle));
        } else {
            const QString code = text(payload, "error");
            setStatus(QStringLiteral("Could not follow @%1: %2")
                          .arg(handle, refreshErrorText(QStringLiteral("mastodon"), code, error)), true);
            if (code == QLatin1String("mastodon_access_token_invalid") ||
                code == QLatin1String("mastodon_access_token_required"))
                promptChannelToken(QStringLiteral("mastodon"));
        }
        if (m_detailChannel == QLatin1String("mastodon"))
            renderMastodon();
    });
}

void MarketingFeature::loadEmailSuggestions()
{
    const QString organization = m_organization->currentData().toString();
    if (!m_installed || organization.isEmpty() || m_emailSuggestionsLoading)
        return;
    m_emailSuggestionsLoading = true;
    m_emailSuggestionsError.clear();
    if (m_emailSuggestionsOrg != organization) {
        m_emailSuggestions = {};
        m_emailSuggestionsOpened.clear();
    }
    m_emailSuggestionsOrg = organization;
    renderEmailSuggestions();
    request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/email-suggestions")), {},
            [this, organization](bool ok, const QJsonObject &payload, const QString &error) {
        m_emailSuggestionsLoading = false;
        if (organization != m_organization->currentData().toString())
            return;
        if (ok)
            m_emailSuggestions = payload.value(QStringLiteral("email")).toObject();
        else
            m_emailSuggestionsError = refreshErrorText(QStringLiteral("email"), text(payload, "error"), error);
        if (m_detailChannel == QLatin1String("email"))
            renderEmailSuggestions();
    });
}

void MarketingFeature::renderEmailSuggestions()
{
    if (!m_detailEmails)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const auto note = [this](const QString &value, const char *object) {
        auto *label = new QLabel(value);
        label->setObjectName(QLatin1String(object));
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        m_detailEmails->addWidget(label);
        return label;
    };
    const bool gmail = text(m_emailSuggestions, "provider") == QLatin1String("gmail");
    const QString mailbox = gmail ? QStringLiteral("Gmail") : QStringLiteral("Zoho Mail");
    const QJsonArray suggestions = m_emailSuggestions.value(QStringLiteral("suggestions")).toArray();

    clearLayout(m_detailEmails);
    if (m_emailSuggestionsLoading && suggestions.isEmpty())
        note(QStringLiteral("Looking through CRM for people to email…"), "marketingHint");
    else if (!m_emailSuggestionsError.isEmpty())
        note(escaped(QStringLiteral("Could not load suggestions: ") + m_emailSuggestionsError), "marketingBad");
    else if (suggestions.isEmpty() && !m_emailSuggestions.isEmpty())
        note(QStringLiteral("Nobody needs an email right now. Suggestions come from CRM: replies not answered yet, "
                            "contacts with no email for 14 days, and new investment or partnership contacts."),
             "marketingHint");
    QList<QWidget *> cards;
    for (const QJsonValue &value : suggestions) {
        const QJsonObject suggestion = value.toObject();
        const QString kind = text(suggestion, "kind");
        const QString contactId = text(suggestion, "contactId");
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("marketingFact"));
        auto *row = new QHBoxLayout(card);
        row->setContentsMargins(8, 6, 8, 6);
        row->setSpacing(8);
        auto *glyph = new QLabel(initials(text(suggestion, "name").isEmpty() ? text(suggestion, "email")
                                                                             : text(suggestion, "name")));
        glyph->setFixedSize(28, 28);
        glyph->setAlignment(Qt::AlignCenter);
        glyph->setStyleSheet(QStringLiteral(
            "background: %1; color: #ffffff; border-radius: 14px; font-size: 11px; font-weight: 600;")
            .arg((kind == QLatin1String("reply") ? colors.warn : brandColor(QStringLiteral("email"), colors)).name()));
        row->addWidget(glyph, 0, Qt::AlignTop);
        const QString kindLabel = kind == QLatin1String("reply") ? QStringLiteral("Reply")
            : kind == QLatin1String("intro") ? QStringLiteral("Introduce")
                                             : QStringLiteral("Follow up");
        QStringList who{text(suggestion, "email")};
        if (!text(suggestion, "company").isEmpty())
            who << text(suggestion, "company");
        QStringList why{kindLabel, text(suggestion, "reason")};
        if (number(suggestion, "at") > 0 && kind != QLatin1String("intro"))
            why << relativeLabel(qint64(number(suggestion, "at")));
        auto *details = new QLabel(
            QStringLiteral("<b>%1</b> <span style=\"color:%2\">%3</span><br>"
                           "<span style=\"color:%2; font-size:11px\">%4</span><br>"
                           "<span style=\"font-size:11px\">%5</span>")
                .arg(escaped(text(suggestion, "name").isEmpty() ? text(suggestion, "email") : text(suggestion, "name")),
                     colors.muted.name(), escaped(who.join(dot)), escaped(why.join(dot)),
                     escaped(text(suggestion, "subject"))));
        details->setObjectName(QStringLiteral("marketingNote"));
        details->setTextFormat(Qt::RichText);
        details->setWordWrap(true);
        details->setToolTip(text(suggestion, "body"));
        details->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row->addWidget(details, 1);
        auto *open = new QPushButton;
        open->setObjectName(QStringLiteral("primaryButton"));
        if (m_emailSuggestionsOpened.contains(contactId)) {
            open->setText(QStringLiteral("Opened ↗"));
            open->setToolTip(QStringLiteral("Opened in %1; Check again to open it once more").arg(mailbox));
            open->setEnabled(false);
        } else {
            open->setText(QStringLiteral("Open in %1 ↗").arg(mailbox));
            open->setCursor(Qt::PointingHandCursor);
            open->setToolTip(QStringLiteral("Open a %1 message to %2 with the subject and text filled in")
                                 .arg(mailbox, text(suggestion, "email")));
            connect(open, &QPushButton::clicked, this, [this, suggestion] { openEmailSuggestion(suggestion); });
        }
        row->addWidget(open, 0, Qt::AlignVCenter);
        cards << card;
    }
    if (!cards.isEmpty()) {
        auto *grid = new TileGrid(420, 8);
        grid->setAccessibleName(QStringLiteral("Suggested emails"));
        grid->setTiles(cards);
        m_detailEmails->addWidget(grid);
    }
    if (!suggestions.isEmpty())
        note(QStringLiteral("Fill in any [bracketed] parts before you send."), "marketingHint");
    m_detailEmails->addStretch(1);
    const int total = int(number(m_emailSuggestions, "total"));
    m_detailEmailsPanel->hint()->setText(m_emailSuggestionsLoading ? QStringLiteral("Looking…")
        : suggestions.isEmpty() ? QString()
        : QStringLiteral("%1 to send%2%3").arg(suggestions.size())
              .arg(total > suggestions.size() ? QStringLiteral(" of %1").arg(total) : QString(),
                   m_emailSuggestionsOpened.isEmpty() ? QString()
                       : QStringLiteral(" · %1 opened").arg(m_emailSuggestionsOpened.size())));
}

void MarketingFeature::loadDiscordPosts(bool force)
{
    const QString organization = m_organization->currentData().toString();
    if (!m_installed || organization.isEmpty() || m_discordPostsLoading)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && m_discordPostsOrg == organization && m_discordPostsAt > 0 &&
        now - m_discordPostsAt < kDiscordPostsFreshMs)
        return;
    m_discordPostsLoading = true;
    m_discordPostsError.clear();
    if (m_discordPostsOrg != organization)
        m_discordPosts = {};
    m_discordPostsOrg = organization;
    renderDiscordPosts();
    request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/discord-posts")), {},
            [this, organization](bool ok, const QJsonObject &payload, const QString &error) {
        m_discordPostsLoading = false;
        if (organization != m_organization->currentData().toString())
            return;
        m_discordPostsAt = QDateTime::currentMSecsSinceEpoch();
        if (ok)
            m_discordPosts = payload.value(QStringLiteral("discord")).toObject();
        else
            m_discordPostsError = refreshErrorText(QStringLiteral("discord"), text(payload, "error"), error);
        if (m_detailChannel == QLatin1String("discord"))
            renderDiscordPosts();
    });
}

void MarketingFeature::renderDiscordPosts()
{
    if (!m_detailDiscord)
        return;
    const QJsonArray posts = m_discordPosts.value(QStringLiteral("posts")).toArray();
    QStringList channels;
    for (const QJsonValue &value : m_discordPosts.value(QStringLiteral("channels")).toArray())
        channels << QStringLiteral("#") + value.toString();
    QString note;
    if (m_discordPostsLoading && posts.isEmpty())
        note = QStringLiteral("Reading the Discord channels…");
    else if (!m_discordPostsError.isEmpty())
        note = QStringLiteral("Could not read the Discord posts: ") + m_discordPostsError;
    else if (posts.isEmpty() && !m_discordPosts.isEmpty())
        note = QStringLiteral("Nothing was posted in %1 in the last %2 days.")
                   .arg(channels.join(QStringLiteral(", ")))
                   .arg(int(number(m_discordPosts, "days")));
    else if (!posts.isEmpty()) {
        note = QStringLiteral("From %1, newest first. Double-click a post to open it in Discord.")
                   .arg(channels.join(QStringLiteral(", ")));
        if (m_discordPosts.value(QStringLiteral("contentMissing")).toBool())
            note += QStringLiteral(" The bot cannot read message text: turn on the Message Content intent "
                                   "for the bot in the Discord developer portal.");
        if (m_discordPosts.value(QStringLiteral("truncated")).toBool())
            note += QStringLiteral(" Only the latest 50 posts of each channel are shown.");
    }
    if (!m_discordPostsLoading && m_discordPostsError.isEmpty() && m_discordPostsAt > 0 &&
        !m_discordPosts.isEmpty())
        note += QStringLiteral(" Read %1.").arg(relativeLabel(m_discordPostsAt));
    m_detailDiscordNote->setText(note);
    m_detailDiscordNote->setVisible(!note.isEmpty());

    m_detailDiscord->setRowCount(0);
    for (const QJsonValue &value : posts) {
        const QJsonObject post = value.toObject();
        const int row = m_detailDiscord->rowCount();
        m_detailDiscord->insertRow(row);
        const qint64 postedAt = qint64(number(post, "postedAt"));
        auto *when = new QTableWidgetItem(timestamp(postedAt));
        when->setData(Qt::UserRole, text(post, "url"));
        when->setToolTip(relativeLabel(postedAt));
        m_detailDiscord->setItem(row, 0, when);
        m_detailDiscord->setItem(row, 1, new QTableWidgetItem(QStringLiteral("#") + text(post, "channel")));
        auto *author = new QTableWidgetItem(text(post, "author") +
            (post.value(QStringLiteral("bot")).toBool() ? QStringLiteral(" (bot)") : QString()));
        m_detailDiscord->setItem(row, 2, author);
        const QString content = text(post, "content");
        auto *message = new QTableWidgetItem(content.isEmpty() ? QStringLiteral("(no text)") : content.simplified() +
            (post.value(QStringLiteral("edited")).toBool() ? QStringLiteral(" (edited)") : QString()));
        message->setToolTip(content);
        m_detailDiscord->setItem(row, 3, message);
    }
    m_detailDiscord->setVisible(!posts.isEmpty());
    fitTable(m_detailDiscord, 12);
    m_detailDiscordPanel->hint()->setText(m_discordPostsLoading ? QStringLiteral("Reading…")
        : posts.isEmpty() ? QString()
        : QStringLiteral("%1 post%2").arg(posts.size()).arg(posts.size() == 1 ? QString() : QStringLiteral("s")));
    renderDiscordChart();
}

void MarketingFeature::renderDiscordChart()
{
    if (!m_detailDiscordChart)
        return;
    const QJsonArray posts = m_discordPosts.value(QStringLiteral("posts")).toArray();
    const int days = qMax(1, int(number(m_discordPosts, "days")));
    const qint64 read = m_discordPostsAt > 0 ? m_discordPostsAt : QDateTime::currentMSecsSinceEpoch();
    const qint64 since = qint64(number(m_discordPosts, "since"));
    const qint64 start = since > 0 ? since : read - days * kDay;
    const qint64 end = start + days * kDay;
    QVector<double> counts(days, 0);
    for (const QJsonValue &value : posts) {
        const qint64 at = qint64(number(value.toObject(), "postedAt"));
        if (at >= start && at <= end)
            counts[qMin(days - 1, int((at - start) / kDay))] += 1;
    }
    LineChart::Series line;
    line.name = QStringLiteral("Posts");
    line.color = brandInk(QStringLiteral("discord"), DashboardColors::from(this));
    for (int day = 0; day < days; ++day) {
        const qint64 at = start + qint64(day) * kDay + kDay / 2;
        line.points.append({at, counts.at(day), counts.at(day) > 0});
        line.history.append({at, counts.at(day), true});
    }
    QList<LineChart::Series> lines;
    if (!m_discordPosts.isEmpty())
        lines.append(line);
    m_detailDiscordChart->setSeries(lines, start, end,
        m_discordPostsLoading ? QStringLiteral("Reading the Discord channels…")
        : m_discordPostsError.isEmpty() ? QStringLiteral("No Discord posts read yet.")
                                        : QStringLiteral("Could not read the Discord posts."));
    m_detailDiscordChartPanel->hint()->setText(m_discordPosts.isEmpty() ? QString()
        : QStringLiteral("%1 post%2 in %3 days · per day%4")
              .arg(posts.size())
              .arg(posts.size() == 1 ? QString() : QStringLiteral("s"))
              .arg(days)
              .arg(m_discordPosts.value(QStringLiteral("truncated")).toBool()
                       ? QStringLiteral(" · latest 50 per channel") : QString()));
}

void MarketingFeature::loadReadyPost()
{
    const QString organization = m_organization->currentData().toString();
    if (!m_installed || organization.isEmpty() || m_readyPostLoading)
        return;
    m_readyPostLoading = true;
    m_readyPostError.clear();
    if (m_readyPostOrg != organization) {
        m_readyPost = {};
        m_readyPostImage = QPixmap();
        m_readyPostImageId.clear();
    }
    m_readyPostOrg = organization;
    m_readyPostSeen = text(m_readyPostOverview.value(QStringLiteral("post")).toObject(), "id");
    renderReadyPost();
    request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/ready-post?channel=x")), {},
            [this, organization](bool ok, const QJsonObject &payload, const QString &error) {
        m_readyPostLoading = false;
        if (organization != m_organization->currentData().toString())
            return;
        if (ok)
            applyReadyPost(payload.value(QStringLiteral("readyPost")).toObject());
        else
            m_readyPostError = readyPostErrorText(text(payload, "error"), error);
        if (m_detailChannel != QLatin1String("x"))
            return;
        renderReadyPost();
        if (!ok)
            return;
        writeReadyPostWhenNeeded();
        if (m_readyPost.value(QStringLiteral("needsPost")).toBool() && agentBusy(readyPostAgent()))
            m_readyPostTimer->start();
    });
}

void MarketingFeature::applyReadyPost(const QJsonObject &ready)
{
    m_readyPost = ready;
    const QJsonObject post = ready.value(QStringLiteral("post")).toObject();
    const QString postId = text(post, "id");
    if (postId == m_readyPostImageId && !m_readyPostImage.isNull())
        return;
    m_readyPostImage = QPixmap();
    m_readyPostImageId = postId;
    const QString dataUrl = text(post, "imageDataUrl");
    const int comma = dataUrl.indexOf(QLatin1Char(','));
    if (dataUrl.startsWith(QLatin1String("data:image/")) && comma > 0 &&
        dataUrl.left(comma).endsWith(QLatin1String(";base64")))
        m_readyPostImage.loadFromData(QByteArray::fromBase64(dataUrl.mid(comma + 1).toLatin1()));
}

void MarketingFeature::renderReadyPost()
{
    if (!m_detailReady)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const auto note = [this](const QString &value, const char *object) {
        auto *label = new QLabel(value);
        label->setObjectName(QLatin1String(object));
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        m_detailReady->addWidget(label);
        return label;
    };
    const QJsonObject ready = m_readyPost.isEmpty() && m_readyPostOrg == m_organization->currentData().toString()
                                  ? m_readyPostOverview : m_readyPost;
    const QJsonObject post = ready.value(QStringLiteral("post")).toObject();
    const QJsonArray history = ready.value(QStringLiteral("history")).toArray();
    const QJsonObject agent = readyPostAgent();
    const QString agentName = agent.isEmpty() || text(agent, "name").isEmpty() ? QStringLiteral("Herald")
                                                                               : text(agent, "name");
    const bool busy = !agent.isEmpty() && agentBusy(agent);
    const bool paused = !agent.isEmpty() && !agent.value(QStringLiteral("enabled")).toBool(true);
    const bool canWrite = canWriteReadyPost(text(organizationRecord(), "role"));

    clearLayout(m_detailReady);
    m_detailReadyPanel->hint()->setText(m_readyPostLoading ? QStringLiteral("Loading…")
        : m_readyPostSaving ? QStringLiteral("Saving…")
        : QStringLiteral("Written by %1 for X").arg(agentName));
    if (!m_readyPostError.isEmpty())
        note(escaped(QStringLiteral("Could not load the ready post: ") + m_readyPostError), "marketingBad");
    if (ready.isEmpty()) {
        if (m_readyPostLoading)
            note(QStringLiteral("Loading the ready X post…"), "marketingHint");
        return;
    }

    if (!post.isEmpty()) {
        const QString postId = text(post, "id");
        const QString body = text(post, "text");
        const QString alt = text(post, "imageAlt");
        const bool hasImage = m_readyPostImageId == postId && !m_readyPostImage.isNull();
        auto *row = new QHBoxLayout;
        row->setSpacing(14);
        if (hasImage) {
            const qreal dpr = devicePixelRatioF();
            QPixmap shown = m_readyPostImage;
            if (shown.width() > 360 * dpr || shown.height() > 450 * dpr) {
                shown = shown.scaled(qRound(360 * dpr), qRound(450 * dpr), Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
                shown.setDevicePixelRatio(dpr);
            }
            auto *image = new QLabel;
            image->setObjectName(QStringLiteral("marketingReadyPostImage"));
            image->setPixmap(shown);
            image->setToolTip(alt);
            image->setAccessibleName(alt.isEmpty() ? QStringLiteral("Image for the X post") : alt);
            row->addWidget(image, 0, Qt::AlignTop);
        }

        auto *about = new QVBoxLayout;
        about->setSpacing(6);
        auto *words = new QLabel(body);
        words->setObjectName(QStringLiteral("marketingReadyPostText"));
        words->setTextFormat(Qt::PlainText);
        words->setWordWrap(true);
        words->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        words->setCursor(Qt::IBeamCursor);
        words->setStyleSheet(QStringLiteral("font-size: 14px;"));
        words->setAccessibleName(QStringLiteral("Ready X post"));
        about->addWidget(words);

        const int length = number(post, "length") > 0 ? int(number(post, "length")) : xLength(body);
        const QColor lengthColor = length > kXLimit ? colors.bad : length > kXLimit - 20 ? colors.warn : colors.muted;
        QStringList facts{QStringLiteral("<span style=\"color:%1; font-weight:600\">%2 / %3</span>")
                              .arg(lengthColor.name()).arg(length).arg(kXLimit)};
        const QString by = text(post, "model").isEmpty() ? text(post, "author") : text(post, "model");
        QString written = QStringLiteral("Written ") + relativeLabel(qint64(number(post, "createdAt")));
        if (!by.isEmpty())
            written += QStringLiteral(" by ") + by;
        facts << escaped(written);
        if (post.value(QStringLiteral("hasImage")).toBool() && !hasImage)
            facts << escaped(m_readyPostLoading ? QStringLiteral("Loading the image…")
                                                : QStringLiteral("The image could not be read"));
        else if (!post.value(QStringLiteral("hasImage")).toBool())
            facts << QStringLiteral("No image");
        auto *meta = new QLabel(facts.join(dot));
        meta->setObjectName(QStringLiteral("marketingHint"));
        meta->setTextFormat(Qt::RichText);
        meta->setWordWrap(true);
        if (length > kXLimit)
            meta->setToolTip(QStringLiteral("Longer than %1: only X Premium accounts can post it whole").arg(kXLimit));
        about->addWidget(meta);
        if (!text(post, "why").isEmpty()) {
            auto *why = new QLabel(escaped(text(post, "why")));
            why->setObjectName(QStringLiteral("marketingHint"));
            why->setTextFormat(Qt::RichText);
            why->setWordWrap(true);
            about->addWidget(why);
        }

        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(6);
        auto *copyText = new QPushButton(QStringLiteral("Copy text"));
        copyText->setObjectName(QStringLiteral("ghostButton"));
        copyText->setToolTip(QStringLiteral("Copy the post's text"));
        connect(copyText, &QPushButton::clicked, this, [this, body] {
            QGuiApplication::clipboard()->setText(body);
            setStatus(QStringLiteral("Copied the X post's text."));
        });
        buttons->addWidget(copyText);
        auto *copyImage = new QPushButton(QStringLiteral("Copy image"));
        copyImage->setObjectName(QStringLiteral("ghostButton"));
        copyImage->setEnabled(hasImage);
        copyImage->setToolTip(hasImage ? QStringLiteral("Copy the image to paste it into the X composer")
                                       : QStringLiteral("This post has no image"));
        connect(copyImage, &QPushButton::clicked, this, [this] {
            if (m_readyPostImage.isNull())
                return;
            QGuiApplication::clipboard()->setImage(m_readyPostImage.toImage());
            setStatus(QStringLiteral("Copied the X post's image."));
        });
        buttons->addWidget(copyImage);
        auto *postNow = new QPushButton(QStringLiteral("Post on X ↗"));
        postNow->setObjectName(QStringLiteral("primaryButton"));
        postNow->setCursor(Qt::PointingHandCursor);
        postNow->setToolTip(hasImage
            ? QStringLiteral("Copy the image, then open the X composer with the text filled in to paste it")
            : QStringLiteral("Open the X composer with the text filled in"));
        connect(postNow, &QPushButton::clicked, this, [this] { postReadyPost(); });
        buttons->addWidget(postNow);
        buttons->addStretch(1);
        auto *posted = new QPushButton(QStringLiteral("Mark posted"));
        posted->setObjectName(QStringLiteral("ghostButton"));
        auto *skip = new QPushButton(QStringLiteral("Skip"));
        skip->setObjectName(QStringLiteral("ghostButton"));
        for (QPushButton *button : {posted, skip}) {
            button->setEnabled(canWrite && !m_readyPostSaving && !m_readyPost.isEmpty());
            buttons->addWidget(button);
        }
        posted->setToolTip(canWrite ? QStringLiteral("You posted it: close it so %1 writes the next one").arg(agentName)
                                    : QStringLiteral("Only organization members can close the ready post"));
        skip->setToolTip(canWrite ? QStringLiteral("Skip this post so %1 writes another one").arg(agentName)
                                  : QStringLiteral("Only organization members can close the ready post"));
        connect(posted, &QPushButton::clicked, this, [this] { closeReadyPost(QStringLiteral("posted")); });
        connect(skip, &QPushButton::clicked, this, [this] { closeReadyPost(QStringLiteral("skipped")); });
        about->addLayout(buttons);
        about->addStretch(1);
        row->addLayout(about, 1);
        m_detailReady->addLayout(row);
        return;
    }

    note(escaped(busy ? QStringLiteral("No post ready yet. %1 is writing the next one.").arg(agentName)
                      : QStringLiteral("No post ready yet. %1 (the X special agent) writes the next one.").arg(agentName)),
         "marketingNote");
    if (!history.isEmpty()) {
        const QJsonObject last = history.at(0).toObject();
        const QString status = text(last, "status");
        const QString label = status == QLatin1String("posted") ? QStringLiteral("Posted")
            : status == QLatin1String("skipped") ? QStringLiteral("Skipped") : QStringLiteral("Replaced");
        QString line = QStringLiteral("Last: <b>%1</b> %2").arg(label,
            escaped(relativeLabel(qint64(number(last, "closedAt")))));
        if (!text(last, "closedBy").isEmpty())
            line += escaped(QStringLiteral(" by ") + text(last, "closedBy"));
        const QString url = text(last, "url");
        if (url.startsWith(QLatin1String("https://")))
            line += QStringLiteral("%1<a style=\"color:%2; text-decoration:none\" href=\"%3\">View ↗</a>")
                        .arg(dot, colors.primary.name(), escaped(url));
        auto *lastLine = note(line, "marketingHint");
        lastLine->setOpenExternalLinks(true);
        lastLine->setToolTip(text(last, "text"));
        if (!text(last, "text").isEmpty())
            note(escaped(QStringLiteral("“") + text(last, "text").simplified().left(160) +
                         (text(last, "text").simplified().size() > 160 ? QStringLiteral("…”") : QStringLiteral("”"))),
                 "marketingHint");
    }
    if (agent.isEmpty())
        note(QStringLiteral("Herald is not set up on this desktop. Add it from the Special agents page and attach "
                            "it to X to always have a post ready."),
             "marketingHint");
    auto *actions = new QHBoxLayout;
    actions->setSpacing(6);
    auto *write = new QPushButton(busy ? QStringLiteral("Writing…") : QStringLiteral("Write one now"));
    write->setObjectName(QStringLiteral("ghostButton"));
    write->setEnabled(!agent.isEmpty() && m_specialAgentAction && !busy && !paused && canWrite);
    write->setToolTip(agent.isEmpty() ? QStringLiteral("Herald is not set up on this desktop")
        : paused ? QStringLiteral("%1 is paused. Turn it on from the Special agents page.").arg(agentName)
        : busy ? QStringLiteral("%1 is already running").arg(agentName)
        : !canWrite ? QStringLiteral("Only organization members can have a post written")
                    : QStringLiteral("Start %1 now to write the next X post").arg(agentName));
    connect(write, &QPushButton::clicked, this, [this] { writeReadyPost(readyPostTask(m_readyPost), false); });
    actions->addWidget(write);
    actions->addStretch(1);
    m_detailReady->addLayout(actions);
}

void MarketingFeature::closeReadyPost(const QString &status)
{
    const QString organization = m_organization->currentData().toString();
    const QString postId = text(m_readyPost.value(QStringLiteral("post")).toObject(), "id");
    if (postId.isEmpty() || m_readyPostSaving || organization != m_readyPostOrg)
        return;
    m_readyPostSaving = true;
    renderReadyPost();
    request(QByteArrayLiteral("PATCH"), organizationPath(QStringLiteral("/ready-post")),
            QJsonObject{{QStringLiteral("channel"), QStringLiteral("x")}, {QStringLiteral("id"), postId},
                        {QStringLiteral("status"), status}},
            [this, organization, status](bool ok, const QJsonObject &payload, const QString &error) {
        m_readyPostSaving = false;
        if (organization != m_organization->currentData().toString())
            return;
        const QString code = text(payload, "error");
        const QJsonObject ready = payload.value(QStringLiteral("readyPost")).toObject();
        if (!ready.isEmpty()) {
            applyReadyPost(ready);
            m_readyPostOverview = ready;
            m_readyPostSeen = text(ready.value(QStringLiteral("post")).toObject(), "id");
        }
        if (ok)
            setStatus(status == QLatin1String("posted") ? QStringLiteral("Marked the X post as posted.")
                                                        : QStringLiteral("Skipped the X post."));
        else if (code == QLatin1String("ready_post_changed"))
            setStatus(QStringLiteral("The ready X post changed in the meantime; here is the new one."), true);
        else if (code == QLatin1String("ready_post_not_found"))
            setStatus(QStringLiteral("That X post was already closed."), true);
        else
            setStatus(QStringLiteral("Could not update the ready X post: ") + readyPostErrorText(code, error), true);
        if (code == QLatin1String("ready_post_not_found"))
            return loadReadyPost();
        if (m_detailChannel != QLatin1String("x"))
            return;
        renderReadyPost();
        if (!ok)
            return;
        writeReadyPostWhenNeeded();
        if (agentBusy(readyPostAgent()))
            m_readyPostTimer->start();
    });
}

void MarketingFeature::postReadyPost()
{
    const QJsonObject post = (m_readyPost.isEmpty() ? m_readyPostOverview : m_readyPost)
                                 .value(QStringLiteral("post")).toObject();
    const QUrl url = QUrl::fromEncoded(text(post, "intentUrl").toUtf8(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") ||
        (url.host() != QLatin1String("x.com") && url.host() != QLatin1String("twitter.com"))) {
        setStatus(QStringLiteral("This post has no X link to open. Copy the text instead."), true);
        return;
    }
    const bool image = m_readyPostImageId == text(post, "id") && !m_readyPostImage.isNull();
    if (image)
        QGuiApplication::clipboard()->setImage(m_readyPostImage.toImage());
    QDesktopServices::openUrl(url);
    setStatus(image ? QStringLiteral("Opened X with the text filled in. The image is on the clipboard: paste it into "
                                     "the composer (%1), post, then press Mark posted here.")
                          .arg(QKeySequence(QKeySequence::Paste).toString(QKeySequence::NativeText))
                    : QStringLiteral("Opened X with the text filled in. Post it, then press Mark posted here."));
}

QJsonObject MarketingFeature::readyPostAgent() const
{
    if (!m_specialAgents)
        return {};
    const QList<QJsonObject> agents = agentsOnChannel(m_specialAgents(), QStringLiteral("x"));
    for (const QJsonObject &agent : agents) {
        if (text(agent, "id") == QLatin1String("herald"))
            return agent;
    }
    return agents.isEmpty() ? QJsonObject() : agents.first();
}

void MarketingFeature::writeReadyPost(const QString &task, bool automatic)
{
    const QString organization = m_organization->currentData().toString();
    const QJsonObject agent = readyPostAgent();
    if (agent.isEmpty() || !m_specialAgentAction || organization.isEmpty())
        return;
    QSettings().setValue(QStringLiteral("marketing/readyPostRunAt/") + organization,
                         QDateTime::currentMSecsSinceEpoch());
    const QString name = text(agent, "name");
    const QString action = text(agent, "workflow").isEmpty() ? QStringLiteral("prompt") : QStringLiteral("run");
    QPointer<MarketingFeature> guard(this);
    m_specialAgentAction(text(agent, "id"), action, task, [guard, automatic, name](bool ok, const QString &message) {
        if (!guard)
            return;
        guard->setStatus(ok && automatic ? QStringLiteral("No X post is ready, so %1 is writing the next one.").arg(name)
                                         : message, !ok);
        if (!guard->m_detailChannel.isEmpty())
            guard->renderChannelAgents(guard->m_detailChannel);
        if (guard->m_detailChannel == QLatin1String("x"))
            guard->renderReadyPost();
        if (ok)
            guard->m_readyPostTimer->start();
    });
}

void MarketingFeature::writeReadyPostWhenNeeded()
{
    const QString organization = m_organization->currentData().toString();
    if (m_detailChannel != QLatin1String("x") || organization.isEmpty() || organization != m_readyPostOrg ||
        !m_readyPost.value(QStringLiteral("needsPost")).toBool() ||
        !canWriteReadyPost(text(organizationRecord(), "role")))
        return;
    const QJsonObject agent = readyPostAgent();
    if (agent.isEmpty() || !m_specialAgentAction || agentBusy(agent) ||
        !agent.value(QStringLiteral("enabled")).toBool(true))
        return;
    const qint64 last = QSettings().value(QStringLiteral("marketing/readyPostRunAt/") + organization).toLongLong();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (last > 0 && last <= now && now - last < kReadyPostRunGap)
        return;
    writeReadyPost(readyPostTask(m_readyPost), true);
}

void MarketingFeature::openEmailSuggestion(const QJsonObject &suggestion)
{
    const QUrl url = QUrl::fromEncoded(text(suggestion, "composeUrl").toUtf8(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) {
        setStatus(QStringLiteral("That email cannot be opened."), true);
        return;
    }
    QDesktopServices::openUrl(url);
    m_emailSuggestionsOpened.insert(text(suggestion, "contactId"));
    setStatus(QString::fromUtf8("Opening an email to %1\xE2\x80\xA6").arg(text(suggestion, "email")));
    renderEmailSuggestions();
}

void MarketingFeature::renderChannelChart()
{
    if (m_detailChannel.isEmpty() || !m_detailChart)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    static const char *keys[] = {"followers", "reach", "engagements", "views", "stars", "forks", "following"};
    static const char *labels[] = {"Followers", "Reach", "Engagements", "Views", "Stars", "Forks", "Following"};
    const bool github = m_detailChannel == QLatin1String("github");
    const QList<QJsonObject> list = seriesByChannel(m_metrics).value(m_detailChannel);
    const bool following = m_detailChannel == QLatin1String("x") ||
        std::any_of(list.cbegin(), list.cend(), [](const QJsonObject &item) { return !missing(item, "following"); });
    for (int id : {4, 5}) {
        if (QAbstractButton *button = m_detailMetric->button(id))
            button->setVisible(github);
    }
    if (QAbstractButton *button = m_detailMetric->button(6))
        button->setVisible(following);
    if (QAbstractButton *button = m_detailMetric->button(3))
        button->setVisible(false);
    const int checked = m_detailMetric->checkedId();
    if (github && m_detailChartChannel != m_detailChannel && m_detailMetric->button(4))
        m_detailMetric->button(4)->setChecked(true);
    else if (((!github && (checked == 4 || checked == 5)) || (!following && checked == 6) || checked == 3) &&
             m_detailMetric->button(0))
        m_detailMetric->button(0)->setChecked(true);
    m_detailChartChannel = m_detailChannel;
    const int metric = qBound(0, m_detailMetric->checkedId(), 6);
    m_detailChartPanel->titleLabel()->setText(QStringLiteral("%1 over time").arg(QLatin1String(labels[metric])));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 start = now - m_rangeDays * kDay;
    LineChart::Series line;
    line.name = channelName(m_detailChannel, m_settings, m_channelCatalog);
    line.color = brandInk(m_detailChannel, colors);
    const QJsonObject carried = snapshotAt(list, start);
    if (!carried.isEmpty())
        line.points.append({start, number(carried, keys[metric]), false});
    for (const QJsonObject &item : list) {
        const qint64 at = qint64(number(item, "observedAt"));
        line.history.append({at, number(item, keys[metric]), true});
        if (at > start)
            line.points.append({at, number(item, keys[metric]), true});
    }
    QList<LineChart::Series> lines;
    if (!line.points.isEmpty())
        lines.append(line);
    m_detailChart->setSeries(lines, start, now,
                             QStringLiteral("No numbers for %1 in the last %2 days.\nRefresh it or log its numbers.")
                                 .arg(line.name).arg(m_rangeDays));

    LineChart::Series views;
    views.name = QStringLiteral("Views");
    views.color = line.color;
    const QJsonObject traffic = channelViews(m_detailChannel);
    if (!traffic.isEmpty()) {
        const QVector<double> days = dailyViews(traffic.value(QStringLiteral("days")).toArray(), start, now, m_rangeDays);
        for (int day = 0; day < days.size(); ++day) {
            const qint64 at = start + qint64(day) * kDay + kDay / 2;
            views.points.append({at, days.at(day), days.at(day) > 0});
            views.history.append({at, days.at(day), true});
        }
        m_detailViewsPanel->hint()->setText(QStringLiteral("%1 views · %2 visitors · %3%4")
            .arg(fullCount(number(traffic, "views")), fullCount(number(traffic, "visitors")),
                 text(traffic, "host"), text(traffic, "path") == QLatin1String("/") ? QString() : text(traffic, "path")));
        m_detailViewsPanel->hint()->setToolTip(QStringLiteral("Page views from Analytics, per day"));
    } else {
        const QJsonObject before = snapshotAt(list, start);
        if (!before.isEmpty() && number(before, "views") > 0)
            views.points.append({start, number(before, "views"), false});
        for (const QJsonObject &item : list) {
            if (missing(item, "views"))
                continue;
            const qint64 at = qint64(number(item, "observedAt"));
            views.history.append({at, number(item, "views"), true});
            if (at > start)
                views.points.append({at, number(item, "views"), true});
        }
        if (std::none_of(views.points.cbegin(), views.points.cend(),
                         [](const LineChart::Point &point) { return point.value > 0; }))
            views.points.clear();
        m_detailViewsPanel->hint()->setText(views.points.isEmpty() ? QString() : QStringLiteral("Recorded numbers"));
        m_detailViewsPanel->hint()->setToolTip(QString());
    }
    QList<LineChart::Series> viewLines;
    if (!views.points.isEmpty())
        viewLines.append(views);
    m_detailViewsChart->setSeries(viewLines, start, now,
                                  QStringLiteral("No views for %1 in the last %2 days.\n"
                                                 "Pages on your Analytics site and recorded numbers show here.")
                                      .arg(line.name).arg(m_rangeDays));
}

QJsonObject MarketingFeature::channelViews(const QString &id) const
{
    return m_traffic.value(QStringLiteral("channelViews")).toObject().value(id).toObject();
}

QVector<double> MarketingFeature::channelSpark(const QString &id, const char *key) const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 start = now - m_rangeDays * kDay;
    const QJsonObject traffic = channelViews(id);
    if (!traffic.isEmpty() && (!key || QLatin1String(key) == QLatin1String("views")))
        return dailyViews(traffic.value(QStringLiteral("days")).toArray(), start, now, qMin(m_rangeDays, 30));
    const QList<QJsonObject> history = seriesByChannel(m_metrics).value(id);
    const char *metric = key;
    if (!metric) {
        for (const char *fallback : {"followers", "views", "reach", "posts"}) {
            if (std::any_of(history.cbegin(), history.cend(),
                            [fallback](const QJsonObject &item) { return number(item, fallback) > 0; })) {
                metric = fallback;
                break;
            }
        }
    }
    QVector<double> values;
    if (!metric)
        return values;
    constexpr int samples = 16;
    for (int sample = 0; sample < samples; ++sample) {
        const QJsonObject snapshot = snapshotAt(history, start + (now - start) * sample / (samples - 1));
        if (!snapshot.isEmpty())
            values.append(number(snapshot, metric));
    }
    return values;
}

void MarketingFeature::activateRailStep(const QString &tile, const QString &step)
{
    if (!m_installed || tile.isEmpty())
        return;
    if (step == QLatin1String("link")) {
        m_railRow = tile;
        m_railRowFocusLink = true;
        showChannelRow(tile, true, true);
    } else {
        openChannel(tile);
        showChannelRow(tile, false, false);
    }
    if (step == QLatin1String("refresh")) {
        refreshChannel(tile);
    } else if (step == QLatin1String("capture")) {
        captureWithExtension(tile);
    } else if (step == QLatin1String("metrics")) {
        addMetric(tile);
    } else if (step == QLatin1String("post")) {
        editContent(QJsonObject{{QStringLiteral("channel"), tile},
                                {QStringLiteral("status"), QStringLiteral("draft")}});
    } else if (step == QLatin1String("enable")) {
        for (const ChannelRow &row : std::as_const(m_channelRows)) {
            if (row.id == tile && row.enabled && row.enabled->isEnabled())
                row.enabled->setChecked(true);
        }
    } else if (step == QLatin1String("open")) {
        for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray()) {
            const QUrl url(channelOpenUrl(value.toObject()));
            if (text(value.toObject(), "id") == tile && url.isValid() &&
                url.scheme() == QLatin1String("https"))
                QDesktopServices::openUrl(url);
        }
    }
}

void MarketingFeature::changeEvent(QEvent *event)
{
    forkmesh::market::NativePackageWidget::changeEvent(event);
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) &&
        !m_themePending) {
        m_themePending = true;
        QTimer::singleShot(0, this, [this] {
            m_themePending = false;
            applyTheme();
        });
    }
}

void MarketingFeature::applyTheme()
{
    const QString css = dashboardStyleSheet(DashboardColors::from(this));
    if (css == m_appliedStyle)
        return;
    m_appliedStyle = css;
    setStyleSheet(css);
    if (m_installed)
        renderDashboard();
}

void MarketingFeature::reveal(QWidget *panel)
{
    if (!panel)
        return;
    closeChannel();
    for (int index = 0; index < m_pages.size(); ++index) {
        QScrollArea *scroll = m_pages.at(index);
        if (!scroll->widget() || !scroll->widget()->isAncestorOf(panel))
            continue;
        showPage(index);
        scroll->widget()->layout()->activate();
        const QPointer<QWidget> target(panel);
        const QPointer<QScrollArea> area(scroll);
        const auto scrollTo = [target, area] {
            if (!target || !area)
                return;
            const int top = target->mapTo(area->widget(), QPoint(0, 0)).y();
            area->verticalScrollBar()->setValue(qMax(0, top - 8));
        };
        scrollTo();
        QTimer::singleShot(0, this, scrollTo);
        return;
    }
}

void MarketingFeature::openWebDashboard(const QString &query)
{
    QUrl url(QString::fromLatin1(kMarketingPage));
    static const QStringList pages{QString(), QStringLiteral("performance"), QStringLiteral("channels"),
                                   QStringLiteral("ads"), QStringLiteral("growth")};
    QString parameters = QStringLiteral("org=") + QString::fromUtf8(
        QUrl::toPercentEncoding(m_organization->currentData().toString()));
    if (!query.isEmpty())
        parameters += QLatin1Char('&') + query;
    else if (m_detailChannel == QLatin1String("reddit"))
        parameters += QStringLiteral("&page=reddit");
    else if (m_page > 0 && m_page < pages.size())
        parameters += QStringLiteral("&page=") + pages.at(m_page);
    url.setQuery(parameters);
    QDesktopServices::openUrl(url);
}

void MarketingFeature::loadTraffic()
{
    if (!m_installed || m_organization->currentData().toString().isEmpty())
        return;
    const int serial = ++m_trafficSerial;
    const qint64 to = QDateTime::currentMSecsSinceEpoch();
    const qint64 from = to - m_rangeDays * kDay;
    request(QByteArrayLiteral("GET"),
            organizationPath(QStringLiteral("/performance")) +
                QStringLiteral("?from=%1&to=%2").arg(from).arg(to),
            {}, [this, serial](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_trafficSerial)
            return;
        m_trafficLoaded = true;
        const QJsonObject performance = payload.value(QStringLiteral("performance")).toObject();
        if (ok && !performance.isEmpty()) {
            m_traffic = performance;
        } else {
            m_traffic = QJsonObject{
                {QStringLiteral("status"), QStringLiteral("error")},
                {QStringLiteral("insights"), QJsonArray{QJsonObject{
                    {QStringLiteral("title"), ok ? QStringLiteral("Analytics has no project for this organization yet.")
                                                 : QStringLiteral("Could not read Analytics: %1").arg(error)}}}}};
        }
        renderKpis();
        renderTraffic();
        renderLanding();
        renderKeywords();
        renderCampaigns();
        renderChannelTiles();
        renderChannelHealth();
        renderChannelDetail();
    });
}

void MarketingFeature::renderDashboard()
{
    if (!m_kpis)
        return;
    renderKpis();
    renderTraffic();
    renderKeywords();
    renderChannelTiles();
    renderCampaigns();
    renderAssistantCampaigns();
    renderAttention();
    renderAssistant();
    renderFlow();
    renderAdsOverview();
    renderX();
    renderWeek();
    renderChart();
    renderLanding();
    renderReferrers();
    renderChannelDetail();
    renderPages();
}

void MarketingFeature::renderKpis()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 range = m_rangeDays * kDay;
    const qint64 start = now - range;
    const MetricSeries series = seriesByChannel(m_metrics);
    const MetricTotals current = totalsAt(series, now);
    QSet<QString> comparable;
    for (auto it = series.cbegin(); it != series.cend(); ++it) {
        if (!snapshotAt(it.value(), start).isEmpty())
            comparable.insert(it.key());
    }
    const MetricTotals before = totalsAt(series, start, &comparable);
    const MetricTotals after = totalsAt(series, now, &comparable);
    const double reach = number(m_summaryData, "reach") > 0 ? number(m_summaryData, "reach") : current.reach;
    const double engagements = number(m_summaryData, "engagements") > 0
        ? number(m_summaryData, "engagements") : current.engagements;
    const double followers = number(m_summaryData, "followers") > 0
        ? number(m_summaryData, "followers") : current.followers;

    const bool ready = text(m_traffic, "status") == QLatin1String("ok");
    const QJsonObject totals = m_traffic.value(QStringLiteral("totals")).toObject();
    const QJsonObject previous = m_traffic.value(QStringLiteral("previous")).toObject();
    const bool goals = !m_traffic.value(QStringLiteral("goals")).toArray().isEmpty();

    KpiStrip::Kpi visits;
    visits.label = QStringLiteral("Visits · %1D").arg(m_rangeDays);
    KpiStrip::Kpi conversions;
    conversions.label = QStringLiteral("Conversions");
    if (ready) {
        visits.value = compactCount(qint64(number(totals, "sessions")));
        visits.tooltip = fullCount(number(totals, "sessions"));
        std::tie(visits.delta, visits.tone) = percentDelta(number(totals, "sessions"), number(previous, "sessions"));
        visits.note = QStringLiteral("%1 visitors").arg(compactCount(qint64(number(totals, "visitors"))));
        if (goals) {
            conversions.value = fullCount(number(totals, "conversions"));
            std::tie(conversions.delta, conversions.tone) =
                percentDelta(number(totals, "conversions"), number(previous, "conversions"));
            conversions.note = QStringLiteral("%1 of visits").arg(shareText(totals.value(QStringLiteral("conversionRate"))));
        } else {
            conversions.value = kDash;
            conversions.note = QStringLiteral("Set a goal on the web dashboard");
        }
    } else {
        visits.value = kDash;
        visits.note = m_trafficLoaded ? QStringLiteral("Analytics not set up") : QStringLiteral("Loading…");
        conversions.value = kDash;
    }

    KpiStrip::Kpi audience;
    audience.label = QStringLiteral("Audience");
    audience.value = compactCount(qint64(followers));
    audience.tooltip = fullCount(followers);
    std::tie(audience.delta, audience.tone) = percentDelta(after.followers, before.followers);
    for (int index = 0; index < 12; ++index)
        audience.spark << totalsAt(series, start + range * index / 11).followers;

    KpiStrip::Kpi reachKpi;
    reachKpi.label = QStringLiteral("Reach");
    reachKpi.value = compactCount(qint64(reach));
    reachKpi.tooltip = fullCount(reach);
    std::tie(reachKpi.delta, reachKpi.tone) = percentDelta(after.reach, before.reach);
    if (reach > 0)
        reachKpi.note = QStringLiteral("%1% engaged").arg(engagements / reach * 100, 0, 'f', 1);

    int published = 0;
    int publishedBefore = 0;
    int upcoming = 0;
    QSet<qint64> covered;
    const qint64 weekEnd = QDateTime(QDate::currentDate().addDays(7), QTime(0, 0)).toMSecsSinceEpoch();
    for (const QJsonValue &value : std::as_const(m_content)) {
        const QJsonObject item = value.toObject();
        const QString status = text(item, "status");
        const qint64 publishedAt = qint64(number(item, "publishedAt"));
        if (status == QLatin1String("published") && publishedAt >= start && publishedAt <= now)
            ++published;
        if (status == QLatin1String("published") && publishedAt >= start - range && publishedAt < start)
            ++publishedBefore;
        const qint64 scheduledAt = qint64(number(item, "scheduledAt"));
        if ((status == QLatin1String("queued") || status == QLatin1String("scheduled")) &&
            scheduledAt >= now && scheduledAt < weekEnd) {
            ++upcoming;
            covered.insert(QDateTime::fromMSecsSinceEpoch(scheduledAt).date().toJulianDay());
        }
    }
    KpiStrip::Kpi publishedKpi;
    publishedKpi.label = QStringLiteral("Published · %1D").arg(m_rangeDays);
    publishedKpi.value = fullCount(published);
    const int diff = published - publishedBefore;
    publishedKpi.delta = diff > 0 ? QStringLiteral("▲ %1").arg(diff)
                       : diff < 0 ? QStringLiteral("▼ %1").arg(-diff) : QStringLiteral("• 0");
    publishedKpi.tone = diff > 0 ? 1 : diff < 0 ? -1 : 0;
    const int openDays = 7 - int(covered.size());
    publishedKpi.note = QStringLiteral("%1 scheduled this week%2").arg(upcoming)
        .arg(openDays > 0 ? QStringLiteral(" · %1 day%2 open").arg(openDays).arg(openDays == 1 ? QString() : QStringLiteral("s"))
                          : QString());

    int live = 0;
    int open = 0;
    QJsonObject ending;
    for (const QJsonValue &value : std::as_const(m_campaigns)) {
        const QJsonObject campaign = value.toObject();
        const QString status = text(campaign, "status");
        open += isOpenCampaign(status) ? 1 : 0;
        if (status != QLatin1String("active"))
            continue;
        ++live;
        const double endsAt = number(campaign, "endsAt");
        if (endsAt > now && (ending.isEmpty() || endsAt < number(ending, "endsAt")))
            ending = campaign;
    }
    KpiStrip::Kpi campaigns;
    campaigns.label = QStringLiteral("Live campaigns");
    campaigns.value = fullCount(live);
    campaigns.note = ending.isEmpty()
        ? QStringLiteral("%1 open").arg(open)
        : QStringLiteral("%1 ends %2").arg(text(ending, "name"), relativeLabel(qint64(number(ending, "endsAt"))));

    m_kpis->setKpis({visits, conversions, audience, reachKpi, publishedKpi, campaigns});
}

void MarketingFeature::renderTraffic()
{
    const DashboardColors colors = DashboardColors::from(this);
    const bool ready = text(m_traffic, "status") == QLatin1String("ok");
    m_trafficTable->clearSpans();
    m_trafficTable->setRowCount(0);
    if (!ready) {
        m_trafficTable->hide();
        QStringList parts;
        for (const QJsonValue &value : m_traffic.value(QStringLiteral("insights")).toArray()) {
            const QJsonObject insight = value.toObject();
            parts << QStringLiteral("<b>%1</b> %2").arg(escaped(text(insight, "title")),
                                                       escaped(text(insight, "detail")));
        }
        m_trafficSetup->setText(m_trafficLoaded
            ? parts.join(QLatin1Char(' ')) + QStringLiteral(" <a style=\"color:%1\" href=\"analytics\">Open Analytics ↗</a>").arg(colors.primary.name())
            : QStringLiteral("Loading Analytics…"));
        m_trafficSetup->show();
        m_trafficPanel->hint()->clear();
        return;
    }
    m_trafficSetup->hide();
    m_trafficTable->show();
    const QJsonArray rows = m_traffic.value(QStringLiteral("channels")).toArray();
    const QJsonObject totals = m_traffic.value(QStringLiteral("totals")).toObject();
    const QJsonObject previous = m_traffic.value(QStringLiteral("previous")).toObject();
    const bool goals = !m_traffic.value(QStringLiteral("goals")).toArray().isEmpty();
    double sum = 0;
    double previousSum = 0;
    for (const QJsonValue &value : rows) {
        sum += number(value.toObject(), "sessions");
        previousSum += number(value.toObject(), "previousSessions");
    }
    sum = sum > 0 ? sum : 1;
    previousSum = previousSum > 0 ? previousSum : 1;
    const QJsonObject project = m_traffic.value(QStringLiteral("project")).toObject();
    m_trafficPanel->hint()->setText(QStringLiteral("%1 · %2 – %3")
        .arg(text(project, "name").isEmpty() ? QStringLiteral("Analytics") : text(project, "name"),
             shortDate(qint64(number(m_traffic, "from"))), shortDate(qint64(number(m_traffic, "to")))));
    const QHash<QString, QColor> swatch{
        {QStringLiteral("organic"), colors.series[0]}, {QStringLiteral("paid"), colors.series[1]},
        {QStringLiteral("social"), colors.series[2]}, {QStringLiteral("referral"), colors.series[3]}};
    const auto changeItem = [&colors](double change, bool known) {
        auto *item = numberItem(!known ? QStringLiteral("new")
                                       : (change > 0 ? QStringLiteral("+") : QString()) + fullCount(change));
        if (known && change > 0)
            item->setForeground(colors.good);
        else if (known && change < 0)
            item->setForeground(colors.bad);
        return item;
    };
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        const int index = m_trafficTable->rowCount();
        m_trafficTable->insertRow(index);
        const QColor color = swatch.value(text(row, "channel"), colors.muted);
        auto *name = new QTableWidgetItem(dotIcon(color), text(row, "label"));
        QStringList sources;
        for (const QJsonValue &source : row.value(QStringLiteral("topSources")).toArray())
            sources << QStringLiteral("%1 %2").arg(text(source.toObject(), "value"),
                                                    fullCount(number(source.toObject(), "count")));
        name->setToolTip(sources.join(QStringLiteral(" · ")));
        m_trafficTable->setItem(index, 0, name);
        const double sessions = number(row, "sessions");
        const double before = number(row, "previousSessions");
        auto *share = new BarRows;
        share->setRows({{QString(), sessions, sum, QStringLiteral("%1%").arg(sessions / sum * 100, 0, 'f', 1), color,
                         before > 0 ? before / previousSum : -1,
                         QStringLiteral("%1% of visits; %2% in the previous period")
                             .arg(sessions / sum * 100, 0, 'f', 1).arg(before / previousSum * 100, 0, 'f', 1)}},
                       0, 48);
        m_trafficTable->setCellWidget(index, 1, share);
        m_trafficTable->setItem(index, 2, numberItem(fullCount(sessions)));
        m_trafficTable->setItem(index, 3, changeItem(sessions - before, before > 0));
        m_trafficTable->setItem(index, 4, numberItem(goals ? shareText(row.value(QStringLiteral("conversionRate"))) : kDash));
        m_trafficTable->setItem(index, 5, numberItem(durationText(number(row, "averageSessionMs"))));
        m_trafficTable->setItem(index, 6, numberItem(shareText(row.value(QStringLiteral("bounceRate")))));
        m_trafficTable->setItem(index, 7, numberItem(QString::number(number(row, "pagesPerSession"), 'f', 2)));
    }
    if (rows.isEmpty()) {
        m_trafficTable->insertRow(0);
        m_trafficTable->setItem(0, 0, new QTableWidgetItem(QStringLiteral("No visits in this window yet.")));
        m_trafficTable->setSpan(0, 0, 1, 8);
    } else {
        const int index = m_trafficTable->rowCount();
        m_trafficTable->insertRow(index);
        QFont bold = m_trafficTable->font();
        bold.setBold(true);
        const double sessions = number(totals, "sessions");
        QList<QTableWidgetItem *> cells{
            new QTableWidgetItem(QStringLiteral("Total")),
            new QTableWidgetItem(QStringLiteral("%1 tagged").arg(shareText(totals.value(QStringLiteral("taggedShare"))))),
            numberItem(fullCount(sessions)),
            changeItem(sessions - number(previous, "sessions"), number(previous, "sessions") > 0),
            numberItem(goals ? shareText(totals.value(QStringLiteral("conversionRate"))) : kDash),
            numberItem(durationText(number(totals, "averageSessionMs"))),
            numberItem(shareText(totals.value(QStringLiteral("bounceRate")))),
            numberItem(QString::number(sessions > 0 ? number(totals, "pageviews") / sessions : 0, 'f', 2))};
        for (int column = 0; column < cells.size(); ++column) {
            cells.at(column)->setFont(bold);
            m_trafficTable->setItem(index, column, cells.at(column));
        }
        cells.at(1)->setForeground(colors.muted);
    }
    fitTable(m_trafficTable, 12);
}

void MarketingFeature::renderKeywords()
{
    const DashboardColors colors = DashboardColors::from(this);
    const bool measuredView = m_keywordView->checkedId() == 1;
    const bool ready = text(m_traffic, "status") == QLatin1String("ok");
    const QJsonArray measured = ready ? m_traffic.value(QStringLiteral("keywords")).toArray() : QJsonArray();
    QHash<QString, QJsonObject> visitsFor;
    for (const QJsonValue &value : measured)
        visitsFor.insert(text(value.toObject(), "term"), value.toObject());
    m_keywordsTable->clear();
    m_keywordsTable->setRowCount(0);
    int shown = 0;
    if (measuredView) {
        m_keywordsTable->setColumnCount(4);
        m_keywordsTable->setHorizontalHeaderLabels(
            {QStringLiteral("Term"), QStringLiteral("Visits"), QStringLiteral("Bounce"), QString()});
        QSet<QString> tracked;
        for (const QJsonValue &value : std::as_const(m_keywords))
            tracked.insert(text(value.toObject(), "term"));
        for (const QJsonValue &value : measured) {
            const QJsonObject row = value.toObject();
            const QString term = text(row, "term");
            const int index = m_keywordsTable->rowCount();
            m_keywordsTable->insertRow(index);
            auto *name = new QTableWidgetItem(term);
            QStringList sources;
            for (const QJsonValue &source : row.value(QStringLiteral("topSources")).toArray())
                sources << text(source.toObject(), "value");
            name->setToolTip(sources.join(QStringLiteral(", ")));
            m_keywordsTable->setItem(index, 0, name);
            m_keywordsTable->setItem(index, 1, numberItem(fullCount(number(row, "sessions"))));
            m_keywordsTable->setItem(index, 2, numberItem(shareText(row.value(QStringLiteral("bounceRate")))));
            if (!tracked.contains(term) && term != QLatin1String("(not provided)")) {
                auto *track = linkButton(QStringLiteral("Track"), QStringLiteral("Track %1").arg(term));
                connect(track, &QPushButton::clicked, this, [this, term] { editKeyword(QJsonObject(), term); });
                m_keywordsTable->setCellWidget(index, 3, track);
            }
            ++shown;
        }
        m_keywordsEmpty->setText(ready
            ? QStringLiteral("No visits carried a search term (utm_term) in this window. Add {keyword} or utm_term to paid search and newsletter links.")
            : QStringLiteral("Search terms come from Analytics once it records visits."));
    } else {
        m_keywordsTable->setColumnCount(5);
        m_keywordsTable->setHorizontalHeaderLabels(
            {QStringLiteral("Keyword"), QStringLiteral("Pos."), QStringLiteral("Impr."),
             QStringLiteral("Clicks"), QStringLiteral("Visits")});
        for (const QJsonValue &value : std::as_const(m_keywords)) {
            const QJsonObject item = value.toObject();
            const int index = m_keywordsTable->rowCount();
            m_keywordsTable->insertRow(index);
            const int priority = qBound(0, int(number(item, "priority")), 3);
            auto *name = new QTableWidgetItem((priority ? QString(priority, QChar(0x2605)) + QLatin1Char(' ') : QString()) +
                                              text(item, "term"));
            name->setData(Qt::UserRole, text(item, "id"));
            const qint64 observedAt = qint64(number(item, "observedAt"));
            QStringList tip;
            if (!text(item, "campaign").isEmpty())
                tip << QStringLiteral("Campaign: ") + text(item, "campaign");
            tip << (observedAt > 0 ? QStringLiteral("Read %1%2").arg(shortDate(observedAt),
                                         text(item, "source").isEmpty() ? QString() : QStringLiteral(" from ") + text(item, "source"))
                                   : QStringLiteral("No reading yet"));
            tip << QStringLiteral("Double-click to edit or log a reading");
            name->setToolTip(tip.join(QLatin1Char('\n')));
            m_keywordsTable->setItem(index, 0, name);
            const double change = number(item, "positionChange");
            auto *position = numberItem(missing(item, "position") ? kDash
                : QString::number(number(item, "position"), 'f', 1) +
                      (change > 0 ? QStringLiteral(" ▲%1").arg(change) : change < 0 ? QStringLiteral(" ▼%1").arg(-change) : QString()));
            if (change > 0)
                position->setForeground(colors.good);
            else if (change < 0)
                position->setForeground(colors.bad);
            m_keywordsTable->setItem(index, 1, position);
            m_keywordsTable->setItem(index, 2, numberItem(missing(item, "impressions") ? kDash
                                                          : compactCount(qint64(number(item, "impressions")))));
            auto *clicks = numberItem(missing(item, "clicks") ? kDash : compactCount(qint64(number(item, "clicks"))));
            if (!missing(item, "ctr"))
                clicks->setToolTip(QStringLiteral("CTR %1").arg(shareText(item.value(QStringLiteral("ctr")))));
            m_keywordsTable->setItem(index, 3, clicks);
            const QJsonObject visits = visitsFor.value(text(item, "term"));
            m_keywordsTable->setItem(index, 4, numberItem(visits.isEmpty() ? kDash
                                                          : compactCount(qint64(number(visits, "sessions")))));
            ++shown;
        }
        m_keywordsEmpty->setText(QStringLiteral(
            "Track the searches you want to win with Add. Import Search Console readings on the web dashboard."));
    }
    m_keywordsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_keywordsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    if (measuredView) {
        m_keywordsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
        m_keywordsTable->setColumnWidth(3, 64);
    }
    m_keywordsEmpty->setVisible(shown == 0);
    m_keywordsTable->setVisible(shown > 0);
    fitTable(m_keywordsTable, 10);
}

void MarketingFeature::renderChannelTiles()
{
    struct Entry {
        QJsonObject item;
        double weight = 0;
        bool paused = false;
    };
    QList<Entry> entries;
    int active = 0;
    for (const QJsonValue &value : m_summaryData.value(QStringLiteral("channels")).toArray()) {
        Entry entry;
        entry.item = value.toObject();
        const QString id = text(entry.item, "channel");
        QJsonObject settings;
        for (const QJsonValue &setting : m_settings.value(QStringLiteral("channels")).toArray()) {
            if (text(setting.toObject(), "id") == id)
                settings = setting.toObject();
        }
        const QJsonObject health = entry.item.value(QStringLiteral("health")).toObject();
        entry.paused = text(health, "state") == QLatin1String("paused") ||
                       !settings.value(QStringLiteral("enabled")).toBool(true);
        active += entry.paused ? 0 : 1;
        for (const char *key : {"stars", "followers", "views", "visits", "outbound", "reach"}) {
            if (entry.weight <= 0)
                entry.weight = number(entry.item, key);
        }
        entries.append(entry);
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.paused != b.paused ? !a.paused : a.weight > b.weight;
    });
    m_tileRefresh.removeIf([](const auto &it) { return it.value().isNull(); });
    QList<QJsonObject> items;
    for (const Entry &entry : std::as_const(entries))
        items << entry.item;
    renderChannelBar(items, active);
    updateRefreshButtons();
}

void MarketingFeature::renderChannelBar(const QList<QJsonObject> &items, int active)
{
    if (!m_channelBar)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    const QJsonObject counts = m_summaryData.value(QStringLiteral("health")).toObject();
    const int good = int(number(counts, "good"));
    const int look = int(number(counts, "attention") + number(counts, "stale"));
    m_channelChips.clear();
    QList<QWidget *> chips;

    auto *all = new ClickFrame;
    all->setObjectName(QStringLiteral("marketingChannelChip"));
    all->setProperty("channel", QString());
    all->setAccessibleName(QStringLiteral("Marketing channel: All"));
    all->setToolTip(QStringLiteral("Every channel on the Dashboard"));
    all->onClicked = [this] { showPage(0); };
    auto *allLayout = new QGridLayout(all);
    allLayout->setContentsMargins(9, 5, 8, 5);
    allLayout->setHorizontalSpacing(6);
    allLayout->setVerticalSpacing(1);
    auto *allName = new QLabel(QStringLiteral("All channels"));
    allName->setObjectName(QStringLiteral("marketingChipName"));
    allLayout->addWidget(allName, 0, 0);
    auto *allFacts = new QLabel(QStringLiteral("<span style=\"color:%1\">%2</span>&nbsp;active · "
                                               "<span style=\"color:%3\">%4</span>&nbsp;on&nbsp;track · "
                                               "<span style=\"color:%5\">%6</span>&nbsp;need&nbsp;a&nbsp;look")
                                    .arg(colors.fg.name()).arg(active)
                                    .arg(colors.good.name()).arg(good)
                                    .arg((look ? colors.warn : colors.fg).name()).arg(look));
    allFacts->setObjectName(QStringLiteral("marketingChipFacts"));
    allFacts->setTextFormat(Qt::RichText);
    allFacts->setWordWrap(true);
    allLayout->addWidget(allFacts, 1, 0);
    auto *allTools = new QHBoxLayout;
    allTools->setContentsMargins(0, 0, 0, 0);
    allTools->setSpacing(0);
    auto *addButton = linkButton(QStringLiteral("+ Add channel"),
        QStringLiteral("Add channels from the catalog: %1 to choose from")
            .arg(m_channelCatalog.isEmpty() ? 100 : m_channelCatalog.size()));
    auto *manageButton = linkButton(QStringLiteral("Manage"), QStringLiteral("Links, display names and posting velocity"));
    allTools->addWidget(addButton);
    allTools->addStretch(1);
    allTools->addWidget(manageButton);
    allLayout->addLayout(allTools, 2, 0);
    allLayout->setColumnStretch(0, 1);
    allLayout->setRowStretch(3, 1);
    connect(addButton, &QPushButton::clicked, this, [this] { reveal(m_catalogPanel); });
    connect(manageButton, &QPushButton::clicked, this, [this] { reveal(m_channelsPanel); });
    m_channelChips << all;
    chips << all;

    const QJsonArray agents = m_specialAgents ? m_specialAgents() : QJsonArray();
    for (const QJsonObject &item : items) {
        QWidget *chip = channelChip(item, agents);
        m_channelChips << static_cast<QFrame *>(chip);
        chips << chip;
    }

    m_channelBar->setTiles(chips);
    applyView();
}

MarketingFeature::ChannelCard MarketingFeature::channelCard(const QJsonObject &item)
{
    const DashboardColors colors = DashboardColors::from(this);
    ChannelCard card;
    const QString id = text(item, "channel");
    card.id = id;
    card.name = channelName(id, m_settings, m_channelCatalog);
    const QString &name = card.name;
    const QJsonObject health = item.value(QStringLiteral("health")).toObject();
    QJsonObject channelSettings;
    for (const QJsonValue &setting : m_settings.value(QStringLiteral("channels")).toArray()) {
        if (text(setting.toObject(), "id") == id)
            channelSettings = setting.toObject();
    }
    card.paused = text(health, "state") == QLatin1String("paused") ||
                  !channelSettings.value(QStringLiteral("enabled")).toBool(true);
    const MetricSeries series = seriesByChannel(m_metrics);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");

    const Headline main = headline(id, item, health);
    const QJsonObject traffic = channelViews(id);
    const bool viewsHeadline = !main.key && number(traffic, "views") > 0;
    card.value = viewsHeadline ? number(traffic, "views") : main.value;
    card.label = viewsHeadline ? QStringLiteral("Views · %1D").arg(m_rangeDays) : main.label;
    card.spark = channelSpark(id, main.key);
    card.sparkTip = !traffic.isEmpty() && (!main.key || QLatin1String(main.key) == QLatin1String("views"))
        ? QStringLiteral("Page views per day, last %1 days").arg(m_rangeDays)
        : QStringLiteral("%1, last %2 days").arg(main.key ? main.label : QStringLiteral("Recorded numbers")).arg(m_rangeDays);
    const double change = number(health, main.key && QLatin1String(main.key) == QLatin1String("stars")
                                             ? "starsChange" : "followersChange");
    card.changeHtml = change == 0 ? QString()
        : QStringLiteral("<span style=\"font-size:11px; color:%1\">%2%3</span>")
              .arg((change > 0 ? colors.good : colors.bad).name(), change > 0 ? QStringLiteral("+") : QStringLiteral("−"),
                   compactCount(qint64(std::abs(change))));

    card.state = text(health, "state");
    QStringList failing;
    for (const QJsonValue &check : health.value(QStringLiteral("checks")).toArray()) {
        if (!check.toObject().value(QStringLiteral("ok")).toBool())
            failing << text(check.toObject(), "label");
    }
    card.stateColor = card.state == QLatin1String("good") ? colors.good
        : card.state == QLatin1String("attention") || card.state == QLatin1String("stale") ? colors.warn
        : card.state == QLatin1String("setup") ? colors.bad : colors.muted;
    card.stateTip = healthLabel(card.state) + (failing.isEmpty() ? QString() : QStringLiteral(": ") + failing.join(QStringLiteral("; ")));

    const auto fact = [&colors](const QString &amount, const QString &unit) {
        return QStringLiteral("<span style=\"color:%1\">%2</span> %3").arg(colors.fg.name(), amount, unit);
    };
    QStringList &facts = card.facts;
    if (number(health, "engagementRate") > 0)
        facts << fact(QString::number(number(health, "engagementRate"), 'f', 1) + QLatin1Char('%'), QStringLiteral("eng."));
    if (id == QLatin1String("email")) {
        facts << fact(fullCount(number(item, "closedDeals")), QStringLiteral("closed deals"));
        if (number(item, "closedValue") > 0)
            facts << fact(compactCount(qint64(number(item, "closedValue"))), QStringLiteral("won"));
        if (number(item, "replies") > 0)
            facts << fact(compactCount(qint64(number(item, "replies"))), QStringLiteral("replies"));
    } else {
        facts << fact(fullCount(number(health, "postsLast30d")), QStringLiteral("posts/30D"));
    }
    if (!viewsHeadline && number(traffic, "views") > 0)
        facts << fact(compactCount(qint64(number(traffic, "views"))), QStringLiteral("views/%1D").arg(m_rangeDays));
    if (id == QLatin1String("github")) {
        if (number(item, "stars") > 0 && number(item, "followers") > 0)
            facts << fact(compactCount(qint64(number(item, "followers"))), QStringLiteral("followers"));
        if (number(item, "forks") > 0)
            facts << fact(compactCount(qint64(number(item, "forks"))), QStringLiteral("forks"));
    }
    const bool reddit = id == QLatin1String("reddit");
    if (reddit && number(item, "followers") > 0 && number(item, "reach") > 0)
        facts << fact(compactCount(qint64(number(item, "reach"))), QStringLiteral("weekly visitors"));
    if (number(item, "engagements") > 0)
        facts << fact(compactCount(qint64(number(item, "engagements"))),
                      reddit ? QStringLiteral("weekly contributions") : QStringLiteral("engaged"));
    if (number(item, "indexedPages") > 0 && number(item, "visits") > 0)
        facts << fact(compactCount(qint64(number(item, "indexedPages"))), QStringLiteral("pages on Google"));
    if (number(item, "rating") > 0)
        facts << fact(QString::number(number(item, "rating"), 'f', 1) + QChar(0x2605), QString());
    if (number(health, "lastPostAt") > 0)
        facts << QStringLiteral("last ") + escaped(relativeLabel(qint64(number(health, "lastPostAt"))));
    const QList<QJsonObject> history = series.value(id);
    if (!history.isEmpty() && text(history.constLast(), "source") == QLatin1String("x-profile")) {
        for (const auto &[key, unit] : {std::pair{"posts", "posts"}, std::pair{"following", "following"},
                                        std::pair{"mentions", "mentions"}}) {
            if (number(history.constLast(), key) > 0)
                facts << fact(compactCount(qint64(number(history.constLast(), key))), QLatin1String(unit));
        }
    }
    int queued = 0;
    for (const QJsonValue &value : std::as_const(m_content)) {
        const QJsonObject content = value.toObject();
        const QString status = text(content, "status");
        if (text(content, "channel") == id &&
            (status == QLatin1String("queued") || status == QLatin1String("scheduled")))
            ++queued;
    }
    if (queued > 0)
        facts << fact(fullCount(queued), QStringLiteral("queued"));
    const QJsonValue age = health.value(QStringLiteral("ageDays"));
    if (age.isDouble() && age.toDouble() >= 7)
        facts << QStringLiteral("<span style=\"color:%1\">numbers %2d old</span>")
                     .arg(colors.warn.name()).arg(qint64(age.toDouble()));

    const QJsonArray guide = health.value(QStringLiteral("guide")).toArray();
    card.guideTip = QStringLiteral("Get %1's numbers:").arg(name);
    for (int index = 0; index < guide.size(); ++index) {
        const QJsonObject guideStep = guide.at(index).toObject();
        const bool done = guideStep.value(QStringLiteral("done")).toBool();
        if (!done && !card.guideStep && !card.paused) {
            card.guideStep = index + 1;
            card.guideSteps = int(guide.size());
            card.guideLabel = text(guideStep, "label");
        }
        card.guideTip += QLatin1Char('\n') + (done ? QString::fromUtf8("\xE2\x9C\x93 ") : QString::fromUtf8("\xE2\x9C\x97 "))
                         + QStringLiteral("%1. ").arg(index + 1) + text(guideStep, "label");
    }

    const QJsonObject edit = m_channelEdits.value(id);
    const QString link = normalizedChannelUrl(edit.contains(QStringLiteral("profileUrl"))
                                                  ? text(edit, "profileUrl") : text(channelSettings, "profileUrl"));
    const QString openLink = link == normalizedChannelUrl(text(channelSettings, "profileUrl"))
        ? channelOpenUrl(channelSettings) : link;
    if (link.startsWith(QLatin1String("https://")))
        card.actions << ChannelAction{QStringLiteral("Open ↗"), openLink, [openLink] { QDesktopServices::openUrl(QUrl(openLink)); }};
    const bool refreshable = m_refreshableChannels.contains(id);
    const bool capturable = link.startsWith(QLatin1String("https://")) &&
                            (!refreshable || id == QLatin1String("google-search") || reddit);
    if (!(refreshable && capturable))
        card.actions << ChannelAction{QStringLiteral("Log"), QStringLiteral("Log %1's numbers by hand").arg(name),
                                      [this, id] { addMetric(id); }};
    if (refreshable)
        card.actions << ChannelAction{QStringLiteral("Refresh"), QString(), [this, id] { refreshChannel(id); }, true};
    if (capturable)
        card.actions << ChannelAction{QStringLiteral("Update"), reddit
            ? QStringLiteral("Update with the browser extension: opens %1 in your browser, where the ForkMesh extension reads the subreddit's members, weekly visitors and weekly contributions.").arg(name)
            : QStringLiteral("Update with the browser extension: opens %1 in your browser, where the ForkMesh extension reads the channel's page and records its numbers.").arg(name),
            [this, id] { captureWithExtension(id); }};
    card.actions << ChannelAction{QStringLiteral("Edit"), QStringLiteral("Link, display name and posting velocity"),
                                  [this, id] { showChannelRow(id, true, true); }};

    card.tip = healthLabel(card.state) + dot +
               (age.isDouble()
                    ? QStringLiteral("numbers from %1 (%2)").arg(agoText(qint64(age.toDouble())),
                                                                sourceLabel(text(health, "source")))
                    : QStringLiteral("no numbers recorded yet"));
    return card;
}

QWidget *MarketingFeature::channelTile(const QJsonObject &item)
{
    const DashboardColors colors = DashboardColors::from(this);
    const ChannelCard card = channelCard(item);
    const QString id = card.id;
    const QString &name = card.name;
    auto *tile = new ClickFrame;
    tile->setObjectName(QStringLiteral("marketingTile"));
    tile->setAccessibleName(name);
    tile->setAccessibleDescription(QStringLiteral("Open %1's page").arg(name));
    tile->onClicked = [this, id] { openChannel(id); };
    auto *layout = new QGridLayout(tile);
    layout->setContentsMargins(12, 10, 8, 6);
    layout->setHorizontalSpacing(10);
    layout->setVerticalSpacing(2);

    auto *glyph = new QLabel(initials(name));
    glyph->setFixedSize(38, 38);
    glyph->setAlignment(Qt::AlignCenter);
    glyph->setStyleSheet(QStringLiteral(
        "background: %1; color: #ffffff; border-radius: 9px; font-size: 12px; font-weight: 600;")
        .arg(brandColor(id, colors).name()));
    layout->addWidget(glyph, 0, 0, 2, 1, Qt::AlignTop);

    auto *valueLabel = new QLabel(compactCount(qint64(card.value)) +
                                  (card.changeHtml.isEmpty() ? QString() : QLatin1Char(' ') + card.changeHtml));
    valueLabel->setObjectName(QStringLiteral("marketingTileValue"));
    valueLabel->setTextFormat(Qt::RichText);
    valueLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    valueLabel->setToolTip(fullCount(card.value));
    valueLabel->setProperty("paused", card.paused);
    auto *valueRow = new QHBoxLayout;
    valueRow->setContentsMargins(0, 0, 0, 0);
    valueRow->setSpacing(8);
    valueRow->addWidget(valueLabel);
    auto *spark = new Sparkline;
    spark->setValues(card.spark, brandInk(id, colors));
    spark->setMaximumWidth(120);
    spark->setAccessibleName(card.sparkTip);
    valueRow->addWidget(spark, 1, Qt::AlignVCenter);
    layout->addLayout(valueRow, 0, 1);

    auto *stateDot = new QLabel;
    stateDot->setFixedSize(8, 8);
    stateDot->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg(card.stateColor.name()));
    stateDot->setToolTip(card.stateTip);
    layout->addWidget(stateDot, 0, 2, Qt::AlignTop | Qt::AlignRight);

    auto *nameLabel = new QLabel(QStringLiteral("<span style=\"color:%1; font-weight:600\">%2</span> · %3")
                                     .arg(colors.fg.name(), escaped(name), escaped(card.label)));
    nameLabel->setObjectName(QStringLiteral("marketingTileLabel"));
    nameLabel->setTextFormat(Qt::RichText);
    nameLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    nameLabel->setProperty("paused", card.paused);
    layout->addWidget(nameLabel, 1, 1, 1, 2);

    auto *factsLabel = new QLabel(card.facts.join(QStringLiteral(" &nbsp; ")));
    factsLabel->setObjectName(QStringLiteral("marketingFacts"));
    factsLabel->setTextFormat(Qt::RichText);
    factsLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    factsLabel->setWordWrap(true);
    layout->addWidget(factsLabel, 2, 0, 1, 3);

    if (card.guideStep) {
        auto *guideLabel = new QLabel(QStringLiteral("<span style=\"color:%1\">Get numbers · step %2 of %3:</span> %4")
                                          .arg(colors.warn.name()).arg(card.guideStep).arg(card.guideSteps)
                                          .arg(escaped(card.guideLabel)));
        guideLabel->setObjectName(QStringLiteral("marketingFacts"));
        guideLabel->setTextFormat(Qt::RichText);
        guideLabel->setTextInteractionFlags(Qt::NoTextInteraction);
        guideLabel->setWordWrap(true);
        guideLabel->setToolTip(card.guideTip);
        layout->addWidget(guideLabel, 3, 0, 1, 3);
    }

    auto *actions = clearWidget();
    auto *actionLayout = new QHBoxLayout(actions);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(0);
    for (const ChannelAction &action : card.actions) {
        auto *button = linkButton(action.label, action.tip);
        connect(button, &QPushButton::clicked, this, action.run);
        if (action.refresh)
            m_tileRefresh.insert(id, button);
        actionLayout->addWidget(button);
    }
    actionLayout->addStretch();
    layout->addWidget(actions, 4, 0, 1, 3);
    layout->setColumnStretch(1, 1);
    layout->setRowStretch(5, 1);

    tile->setToolTip(card.tip + QStringLiteral("\nClick for %1's page").arg(name));
    return tile;
}

QWidget *MarketingFeature::channelChip(const QJsonObject &item, const QJsonArray &agents)
{
    const DashboardColors colors = DashboardColors::from(this);
    const ChannelCard card = channelCard(item);
    const QString id = card.id;
    auto *chip = new ClickFrame;
    chip->setObjectName(QStringLiteral("marketingChannelChip"));
    chip->setProperty("channel", id);
    chip->setProperty("paused", card.paused);
    chip->setStyleSheet(chipToneSheet(card.state, card.paused, colors));
    chip->setAccessibleName(QStringLiteral("Marketing channel: ") + card.name);
    chip->setAccessibleDescription(QStringLiteral("Open %1's page").arg(card.name));
    chip->onClicked = [this, id] { openChannel(id); };
    auto *layout = new QGridLayout(chip);
    layout->setContentsMargins(9, 5, 2, 5);
    layout->setHorizontalSpacing(5);
    layout->setVerticalSpacing(1);

    auto *swatch = new QLabel;
    swatch->setFixedSize(9, 9);
    swatch->setStyleSheet(QStringLiteral("background: %1; border-radius: 3px;").arg(brandColor(id, colors).name()));
    layout->addWidget(swatch, 0, 0, Qt::AlignVCenter);

    auto *nameLabel = new QLabel(card.name);
    nameLabel->setObjectName(QStringLiteral("marketingChipName"));
    nameLabel->setProperty("paused", card.paused);
    layout->addWidget(nameLabel, 0, 1);

    const QList<QJsonObject> working = agentsOnChannel(agents, id);
    QStringList agentNames;
    for (const QJsonObject &agent : working)
        agentNames << text(agent, "name");
    if (!working.isEmpty()) {
        auto *faces = new QHBoxLayout;
        faces->setContentsMargins(0, 0, 0, 0);
        faces->setSpacing(2);
        for (const QJsonObject &agent : working.mid(0, 3)) {
            QLabel *face = agentFace(agent, 16, colors, this);
            const QString specialty = text(agent, "specialty");
            face->setToolTip(QStringLiteral("Special agent: %1%2").arg(
                text(agent, "name"), specialty.isEmpty() ? QString() : QStringLiteral(" · ") + specialty));
            faces->addWidget(face);
        }
        chip->setAccessibleDescription(QStringLiteral("Open %1's page · worked by %2")
                                           .arg(card.name, agentNames.join(QStringLiteral(", "))));
        layout->addLayout(faces, 0, 2, Qt::AlignVCenter);
    }

    auto *valueLabel = new QLabel(compactCount(qint64(card.value)) +
                                  (card.changeHtml.isEmpty() ? QString() : QLatin1Char(' ') + card.changeHtml));
    valueLabel->setObjectName(QStringLiteral("marketingChipValue"));
    valueLabel->setTextFormat(Qt::RichText);
    valueLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    valueLabel->setToolTip(fullCount(card.value) + QStringLiteral(" · ") + card.label);
    valueLabel->setProperty("paused", card.paused);
    layout->addWidget(valueLabel, 0, 3, Qt::AlignRight);

    auto *stateDot = new QLabel;
    stateDot->setFixedSize(7, 7);
    stateDot->setStyleSheet(QStringLiteral("background: %1; border-radius: 3px;").arg(card.stateColor.name()));
    stateDot->setToolTip(card.stateTip);
    layout->addWidget(stateDot, 0, 4, Qt::AlignVCenter);

    const QList<ChannelAction> actions = card.actions;
    const auto showMenu = [this, id, actions](const QPoint &at) {
        const QList<ChannelAction> choices = actions;
        QMenu menu;
        QList<QAction *> entries;
        for (const ChannelAction &action : choices) {
            QAction *entry = menu.addAction(action.label);
            entry->setToolTip(action.tip);
            if (action.refresh) {
                for (const ChannelRow &row : std::as_const(m_channelRows)) {
                    if (row.id == id && row.refresh) {
                        entry->setText(row.refresh->text());
                        entry->setEnabled(row.refresh->isEnabled());
                    }
                }
            }
            entries << entry;
        }
        menu.setToolTipsVisible(true);
        const int chosen = int(entries.indexOf(menu.exec(at)));
        if (chosen >= 0)
            choices.at(chosen).run();
    };
    auto *more = linkButton(QString::fromUtf8("\xE2\x8B\xAF"), QStringLiteral("Open, log, refresh or edit %1").arg(card.name));
    connect(more, &QPushButton::clicked, this, [more, showMenu] {
        const QPoint at = more->mapToGlobal(QPoint(0, more->height()));
        showMenu(at);
    });
    layout->addWidget(more, 0, 5, Qt::AlignVCenter);
    chip->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(chip, &QWidget::customContextMenuRequested, this, [chip, showMenu](const QPoint &pos) {
        const QPoint at = chip->mapToGlobal(pos);
        showMenu(at);
    });

    QStringList facts{escaped(card.label)};
    facts << card.facts;
    if (card.guideStep)
        facts << QStringLiteral("<span style=\"color:%1\">setup %2/%3</span>")
                     .arg(colors.warn.name()).arg(card.guideStep).arg(card.guideSteps);
    for (QString &fact : facts) {
        QString kept;
        bool tag = false;
        for (const QChar c : std::as_const(fact)) {
            tag = c == QLatin1Char('<') || (tag && c != QLatin1Char('>'));
            kept += tag ? QString(c)
                : c == QLatin1Char(' ') ? QStringLiteral("&nbsp;")
                : c == QLatin1Char('/') ? QStringLiteral("/&#8288;") : QString(c);
        }
        fact = kept;
    }
    auto *factsLabel = new QLabel(facts.join(QStringLiteral(" · ")));
    factsLabel->setObjectName(QStringLiteral("marketingChipFacts"));
    factsLabel->setTextFormat(Qt::RichText);
    factsLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    factsLabel->setWordWrap(true);
    layout->addWidget(factsLabel, 1, 0, 1, 6);
    auto *spark = new Sparkline;
    spark->setValues(card.spark, brandInk(id, colors));
    spark->setAccessibleName(card.sparkTip);
    layout->addWidget(spark, 3, 0, 1, 6, Qt::AlignBottom);
    layout->setColumnStretch(1, 1);
    layout->setRowStretch(2, 1);

    QString tip = card.name + QStringLiteral(" · ") + card.tip;
    if (card.guideStep)
        tip += QStringLiteral("\nNext: ") + card.guideLabel + QLatin1Char('\n') + card.guideTip;
    if (!agentNames.isEmpty())
        tip += QStringLiteral("\nSpecial agent: ") + agentNames.join(QStringLiteral(", "));
    chip->setToolTip(tip + QStringLiteral("\nClick for %1's page · right-click for actions").arg(card.name));
    return chip;
}

QJsonObject MarketingFeature::campaignById(const QString &id) const
{
    for (const QJsonValue &value : m_campaigns) {
        if (!id.isEmpty() && text(value.toObject(), "id") == id)
            return value.toObject();
    }
    return {};
}

QJsonObject MarketingFeature::campaignResults(const QJsonObject &campaign) const
{
    if (text(m_traffic, "status") != QLatin1String("ok") || text(campaign, "slug").isEmpty())
        return {};
    double sessions = 0;
    double previous = 0;
    double conversions = 0;
    for (const QJsonValue &value : m_traffic.value(QStringLiteral("campaigns")).toArray()) {
        const QJsonObject row = value.toObject();
        if (text(row, "campaign") != text(campaign, "slug"))
            continue;
        sessions += number(row, "sessions");
        previous += number(row, "previousSessions");
        conversions += number(row, "conversions");
    }
    QJsonObject result{{QStringLiteral("sessions"), sessions}, {QStringLiteral("previous"), previous}};
    if (!m_traffic.value(QStringLiteral("goals")).toArray().isEmpty())
        result.insert(QStringLiteral("conversions"), conversions);
    return result;
}

void MarketingFeature::renderCampaigns()
{
    const DashboardColors colors = DashboardColors::from(this);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int openCount = 0;
    int doneCount = 0;
    for (const QJsonValue &value : std::as_const(m_campaigns)) {
        const QString status = text(value.toObject(), "status");
        openCount += isOpenCampaign(status) ? 1 : 0;
        doneCount += status == QLatin1String("completed") ? 1 : 0;
    }
    m_campaignTab->button(0)->setText(QStringLiteral("Open %1").arg(openCount));
    m_campaignTab->button(1)->setText(QStringLiteral("Done %1").arg(doneCount));
    m_campaignTab->button(2)->setText(QStringLiteral("All %1").arg(m_campaigns.size()));
    const int tab = m_campaignTab->checkedId();
    static const QStringList order{QStringLiteral("active"), QStringLiteral("paused"), QStringLiteral("planned"),
                                   QStringLiteral("idea"), QStringLiteral("completed"), QStringLiteral("archived")};
    QList<QJsonObject> shown;
    QSet<QString> sources;
    for (const QJsonValue &value : std::as_const(m_campaigns)) {
        const QJsonObject campaign = value.toObject();
        const QString status = text(campaign, "status");
        if (!text(campaign, "source").isEmpty())
            sources.insert(text(campaign, "source"));
        if (tab == 2 || (tab == 1 ? status == QLatin1String("completed") : isOpenCampaign(status)))
            shown.append(campaign);
    }
    std::stable_sort(shown.begin(), shown.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const int left = order.indexOf(text(a, "status"));
        const int right = order.indexOf(text(b, "status"));
        if (left != right)
            return left < right;
        const double aEnd = number(a, "endsAt") > 0 ? number(a, "endsAt") : 1e18;
        const double bEnd = number(b, "endsAt") > 0 ? number(b, "endsAt") : 1e18;
        return aEnd < bEnd;
    });
    const auto datesText = [now](const QJsonObject &item) {
        const qint64 startsAt = qint64(number(item, "startsAt"));
        const qint64 endsAt = qint64(number(item, "endsAt"));
        if (startsAt <= 0 && endsAt <= 0)
            return QStringLiteral("No dates");
        const QString range = (startsAt > 0 ? shortDate(startsAt) : QStringLiteral("…")) + QStringLiteral(" – ") +
                              (endsAt > 0 ? shortDate(endsAt) : QStringLiteral("…"));
        const int left = int(number(item.value(QStringLiteral("stats")).toObject(), "daysLeft"));
        if (text(item, "status") == QLatin1String("active") && left > 0)
            return range + QStringLiteral(" · %1 day%2 left").arg(left).arg(left == 1 ? QString() : QStringLiteral("s"));
        if (startsAt > now)
            return range + QStringLiteral(" · starts ") + relativeLabel(startsAt);
        return range;
    };
    const auto pill = [](const QString &label, const QString &tone) {
        auto *widget = new QLabel(label);
        widget->setObjectName(QStringLiteral("marketingPill"));
        widget->setProperty("tone", tone);
        return widget;
    };
    const auto statusTone = [](const QString &status) {
        if (status == QLatin1String("active"))
            return QStringLiteral("good");
        if (status == QLatin1String("paused"))
            return QStringLiteral("warn");
        if (status == QLatin1String("planned"))
            return QStringLiteral("primary");
        if (status == QLatin1String("idea"))
            return QStringLiteral("idea");
        return QString();
    };
    const auto cardFrame = [](bool ghost) {
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("marketingCard"));
        card->setProperty("ghost", ghost);
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(12, 8, 12, 4);
        layout->setSpacing(5);
        return std::pair{card, layout};
    };
    const auto head = [](QVBoxLayout *layout, const QString &title, const QString &detail, QLabel *badge) {
        auto *row = new QHBoxLayout;
        auto *names = new QVBoxLayout;
        names->setSpacing(1);
        auto *name = new QLabel(title);
        name->setObjectName(QStringLiteral("marketingCardTitle"));
        name->setTextFormat(Qt::PlainText);
        name->setWordWrap(true);
        names->addWidget(name);
        auto *small = new QLabel(detail);
        small->setObjectName(QStringLiteral("marketingSmall"));
        small->setTextFormat(Qt::PlainText);
        small->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        names->addWidget(small);
        row->addLayout(names, 1);
        row->addWidget(badge, 0, Qt::AlignTop);
        layout->addLayout(row);
    };
    const auto paragraph = [](QVBoxLayout *layout, const QString &value, const char *objectName) {
        auto *label = new QLabel(value.size() > 200 ? value.left(197) + QStringLiteral("…") : value);
        label->setObjectName(QLatin1String(objectName));
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setToolTip(value.size() > 200 ? value : QString());
        layout->addWidget(label);
    };
    const QString rangeLabel = QStringLiteral("Visits · %1D").arg(m_rangeDays);
    QList<QWidget *> cards;
    for (const QJsonObject &item : std::as_const(shown)) {
        const QString id = text(item, "id");
        const QJsonObject stats = item.value(QStringLiteral("stats")).toObject();
        const QJsonObject results = campaignResults(item);
        auto [card, layout] = cardFrame(false);
        card->setAccessibleName(text(item, "name"));
        head(layout, text(item, "name"),
             datesText(item) + (text(item, "owner").isEmpty() ? QString() : QStringLiteral(" · ") + text(item, "owner")),
             pill(choiceLabel(campaignStatuses(), text(item, "status")), statusTone(text(item, "status"))));
        if (stats.value(QStringLiteral("timeProgress")).isDouble()) {
            auto *timeline = new ProgressLine;
            timeline->setProgress(number(stats, "timeProgress"),
                                  text(item, "status") == QLatin1String("active") ? colors.good : colors.muted);
            timeline->setToolTip(QStringLiteral("%1% of the flight").arg(qRound(number(stats, "timeProgress") * 100)));
            layout->addWidget(timeline);
        }
        if (!text(item, "message").isEmpty())
            paragraph(layout, text(item, "message"), "marketingHint");
        QList<BarRows::Row> bullets;
        const auto bullet = [&bullets, &colors](const QString &label, const QJsonValue &value, const QJsonValue &target) {
            const bool hasValue = value.isDouble();
            const bool hasTarget = target.isDouble() && target.toDouble() > 0;
            if (!hasValue && !hasTarget)
                return;
            const double current = value.toDouble();
            const bool met = hasTarget && current >= target.toDouble();
            bullets.append({label, current, hasTarget ? target.toDouble() : 0,
                            (hasValue ? fullCount(current) : kDash) +
                                (hasTarget ? QStringLiteral(" / ") + fullCount(target.toDouble()) : QString()),
                            met ? colors.good : colors.series[0], hasTarget ? 1.0 : -1.0, QString()});
        };
        bullet(rangeLabel, results.value(QStringLiteral("sessions")), item.value(QStringLiteral("targetVisits")));
        bullet(QStringLiteral("Conversions"), results.value(QStringLiteral("conversions")),
               item.value(QStringLiteral("targetConversions")));
        bullet(QStringLiteral("Published"), QJsonValue(number(stats, "published")),
               number(stats, "content") > 0 ? QJsonValue(number(stats, "content")) : QJsonValue());
        if (number(stats, "posts") > 0)
            bullet(QStringLiteral("Posted links"), QJsonValue(number(stats, "posts")), QJsonValue());
        if (!item.value(QStringLiteral("tasks")).toArray().isEmpty())
            bullet(QStringLiteral("Checklist"), QJsonValue(number(stats, "tasksDone")), QJsonValue(number(stats, "tasksTotal")));
        if (!bullets.isEmpty()) {
            auto *bars = new BarRows;
            bars->setRows(bullets, 92, 84);
            layout->addWidget(bars);
        }
        QSet<QString> uncovered;
        for (const QJsonValue &value : stats.value(QStringLiteral("uncoveredChannels")).toArray())
            uncovered.insert(value.toString());
        QStringList chips;
        for (const QJsonValue &value : item.value(QStringLiteral("channels")).toArray()) {
            const QString channel = value.toString();
            const QString label = escaped(channelName(channel, m_settings, m_channelCatalog));
            chips << (uncovered.contains(channel)
                ? QStringLiteral("<span style=\"color:%1\">○&nbsp;%2</span>").arg(colors.muted.name(), label)
                : QStringLiteral("<span style=\"color:%1\">●</span>&nbsp;%2").arg(brandInk(channel, colors).name(), label));
        }
        if (!chips.isEmpty()) {
            auto *chipLabel = new QLabel(chips.join(QStringLiteral(" &nbsp; ")));
            chipLabel->setObjectName(QStringLiteral("marketingNote"));
            chipLabel->setTextFormat(Qt::RichText);
            chipLabel->setWordWrap(true);
            chipLabel->setToolTip(QStringLiteral("○ marks a channel with no content planned yet"));
            layout->addWidget(chipLabel);
        }
        if (number(stats, "nextScheduledAt") > 0) {
            auto *next = new QLabel(QStringLiteral("Next: <b>%1</b> · %2")
                                        .arg(escaped(text(stats, "nextTitle")),
                                             escaped(relativeLabel(qint64(number(stats, "nextScheduledAt"))))));
            next->setObjectName(QStringLiteral("marketingHint"));
            next->setTextFormat(Qt::RichText);
            next->setWordWrap(true);
            layout->addWidget(next);
        }
        const QJsonArray tasks = item.value(QStringLiteral("tasks")).toArray();
        int listed = 0;
        for (int index = 0; index < tasks.size() && listed < 3; ++index) {
            const QJsonObject task = tasks.at(index).toObject();
            if (task.value(QStringLiteral("done")).toBool())
                continue;
            auto *check = new QCheckBox(text(task, "title"));
            check->setObjectName(QStringLiteral("marketingTask"));
            connect(check, &QCheckBox::toggled, this,
                    [this, id, index](bool done) { toggleCampaignTask(id, index, done); });
            layout->addWidget(check);
            ++listed;
        }
        QStringList line;
        QStringList spend;
        for (const QJsonValue &value : stats.value(QStringLiteral("spend")).toArray())
            spend << money(value.toObject().value(QStringLiteral("cents")), text(value.toObject(), "currency"));
        if (!spend.isEmpty())
            line << QStringLiteral("Spent ") + spend.join(QStringLiteral(" · "));
        if (number(item, "budgetCents") > 0)
            line << QStringLiteral("budget ") + money(item.value(QStringLiteral("budgetCents")), text(item, "currency"));
        if (number(stats, "ads") > 0)
            line << QStringLiteral("%1 ad%2").arg(int(number(stats, "ads"))).arg(number(stats, "ads") == 1 ? QString() : QStringLiteral("s"));
        if (!line.isEmpty())
            paragraph(layout, line.join(QStringLiteral(" · ")), "marketingSmall");
        layout->addStretch(1);
        auto *foot = new QFrame;
        foot->setObjectName(QStringLiteral("marketingCardFoot"));
        auto *footLayout = new QHBoxLayout(foot);
        footLayout->setContentsMargins(0, 4, 0, 0);
        footLayout->setSpacing(0);
        auto *edit = linkButton(QStringLiteral("Edit"));
        connect(edit, &QPushButton::clicked, this, [this, id] { editCampaign(campaignById(id)); });
        footLayout->addWidget(edit);
        auto *draft = linkButton(QStringLiteral("Draft post"));
        connect(draft, &QPushButton::clicked, this, [this, item] {
            const QJsonArray channels = item.value(QStringLiteral("channels")).toArray();
            editContent(QJsonObject{{QStringLiteral("campaign"), text(item, "name")},
                                    {QStringLiteral("channel"), channels.isEmpty() ? QString() : channels.first().toString()},
                                    {QStringLiteral("status"), QStringLiteral("draft")}});
        });
        footLayout->addWidget(draft);
        if (m_assistantAvailable) {
            auto *plan = linkButton(QStringLiteral("Ask agent"), QStringLiteral("Plan this campaign with the Marketing agent"));
            connect(plan, &QPushButton::clicked, this, [this, id] {
                reveal(m_agentPanel);
                runAssistant(id);
            });
            footLayout->addWidget(plan);
        }
        auto *brief = linkButton(QStringLiteral("Copy brief"), QStringLiteral("Copy the campaign brief as Markdown"));
        connect(brief, &QPushButton::clicked, this, [this, id] {
            QGuiApplication::clipboard()->setText(campaignBrief(campaignById(id)));
            setStatus(QStringLiteral("Campaign brief copied. Paste it into a Marketing agent run or a task."));
        });
        footLayout->addWidget(brief);
        if (!text(stats, "taggedLink").isEmpty()) {
            const QString tagged = text(stats, "taggedLink");
            auto *copyLink = linkButton(QStringLiteral("Copy link"), tagged);
            connect(copyLink, &QPushButton::clicked, this, [this, tagged] {
                QGuiApplication::clipboard()->setText(tagged);
                setStatus(QStringLiteral("Tagged link copied: %1").arg(tagged));
            });
            footLayout->addWidget(copyLink);
        }
        footLayout->addStretch();
        layout->addWidget(foot);
        cards << card;
    }
    for (const QJsonValue &value : std::as_const(m_campaignTemplates)) {
        const QJsonObject preset = value.toObject();
        if (sources.contains(text(preset, "source")))
            continue;
        auto [card, layout] = cardFrame(true);
        head(layout, text(preset, "name"),
             QStringLiteral("%1 · %2 – %3").arg(text(preset, "label"), shortDate(qint64(number(preset, "startsAt"))),
                                                shortDate(qint64(number(preset, "endsAt")))),
             pill(QStringLiteral("Template"), QString()));
        if (!text(preset, "message").isEmpty())
            paragraph(layout, text(preset, "message"), "marketingHint");
        layout->addStretch(1);
        auto *foot = new QFrame;
        foot->setObjectName(QStringLiteral("marketingCardFoot"));
        auto *footLayout = new QHBoxLayout(foot);
        footLayout->setContentsMargins(0, 4, 0, 0);
        auto *use = linkButton(QStringLiteral("Use template"));
        connect(use, &QPushButton::clicked, this, [this, preset] { editCampaign(QJsonObject(), preset); });
        footLayout->addWidget(use);
        footLayout->addStretch();
        layout->addWidget(foot);
        cards << card;
    }
    if (cards.isEmpty()) {
        auto [card, layout] = cardFrame(true);
        auto *empty = new QLabel(QStringLiteral("No campaigns here. New campaign plans one with dates, channels, targets and a checklist."));
        empty->setObjectName(QStringLiteral("marketingHint"));
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignCenter);
        layout->addWidget(empty);
        cards << card;
    }
    m_campaignGrid->setTiles(cards);
    renderAssistantCampaigns();
}

QString MarketingFeature::campaignBrief(const QJsonObject &item) const
{
    const QJsonObject stats = item.value(QStringLiteral("stats")).toObject();
    const QJsonObject results = campaignResults(item);
    const auto day = [](double at) {
        return at > 0 ? QDateTime::fromMSecsSinceEpoch(qint64(at)).toString(QStringLiteral("yyyy-MM-dd"))
                      : QStringLiteral("not set");
    };
    const auto cell = [](QString value) {
        value = value.isEmpty() ? kDash : value;
        return value.replace(QLatin1Char('|'), QStringLiteral("\\|")).replace(QLatin1Char('\n'), QLatin1Char(' '));
    };
    const auto joined = [](const QJsonArray &values, const QString &separator) {
        QStringList parts;
        for (const QJsonValue &value : values)
            parts << value.toString();
        return parts.join(separator);
    };
    QStringList channels;
    for (const QJsonValue &value : item.value(QStringLiteral("channels")).toArray())
        channels << channelName(value.toString(), m_settings, m_channelCatalog);
    QStringList uncovered;
    for (const QJsonValue &value : stats.value(QStringLiteral("uncoveredChannels")).toArray())
        uncovered << channelName(value.toString(), m_settings, m_channelCatalog);
    QStringList targets;
    if (number(item, "targetVisits") > 0)
        targets << QStringLiteral("%1 visits").arg(fullCount(number(item, "targetVisits")));
    if (number(item, "targetConversions") > 0)
        targets << QStringLiteral("%1 conversions").arg(fullCount(number(item, "targetConversions")));
    QStringList spend;
    for (const QJsonValue &value : stats.value(QStringLiteral("spend")).toArray())
        spend << money(value.toObject().value(QStringLiteral("cents")), text(value.toObject(), "currency"));
    QStringList lines{
        QStringLiteral("# ") + text(item, "name"), QString(),
        QStringLiteral("**Owner:** %1 · **Status:** %2 · **Objective:** %3 · **Campaign tag:** %4")
            .arg(text(item, "owner").isEmpty() ? QStringLiteral("not set") : text(item, "owner"), text(item, "status"),
                 text(item, "objective"), text(item, "slug").isEmpty() ? QStringLiteral("none") : text(item, "slug")),
        QStringLiteral("**Dates:** %1 to %2").arg(day(number(item, "startsAt")), day(number(item, "endsAt"))), QString(),
        QStringLiteral("## Brief"), QString(), QStringLiteral("| Field | Value |"), QStringLiteral("|---|---|"),
        QStringLiteral("| Audience | %1 |").arg(cell(text(item, "audience"))),
        QStringLiteral("| Message | %1 |").arg(cell(text(item, "message"))),
        QStringLiteral("| Offer | %1 |").arg(cell(text(item, "offer"))),
        QStringLiteral("| Channels | %1 |").arg(cell(channels.join(QStringLiteral(", ")))),
        QStringLiteral("| Success measure | %1 |").arg(cell(targets.join(QStringLiteral(", ")))),
        QStringLiteral("| Landing page | %1 |").arg(cell(text(item, "landingUrl"))),
        QStringLiteral("| Hashtags | %1 |").arg(cell(joined(item.value(QStringLiteral("hashtags")).toArray(), QStringLiteral(" ")))),
        QStringLiteral("| Search keywords | %1 |").arg(cell(joined(item.value(QStringLiteral("keywords")).toArray(), QStringLiteral(", ")))),
        QStringLiteral("| Budget | %1 |").arg(cell(number(item, "budgetCents") > 0
            ? money(item.value(QStringLiteral("budgetCents")), text(item, "currency")) : QString())),
        QString()};
    if (!text(stats, "taggedLink").isEmpty())
        lines << QStringLiteral("## Tagged link") << QString() << text(stats, "taggedLink") << QString();
    lines << QStringLiteral("## Where it stands") << QString()
          << QStringLiteral("- Content: %1 items, %2 published%3.").arg(int(number(stats, "content"))).arg(int(number(stats, "published")))
                 .arg(text(stats, "nextTitle").isEmpty() ? QStringLiteral("; nothing scheduled")
                      : QStringLiteral("; next “%1” on %2").arg(text(stats, "nextTitle"), day(number(stats, "nextScheduledAt"))))
          << QStringLiteral("- Channels without content: %1.").arg(uncovered.isEmpty() ? QStringLiteral("none") : uncovered.join(QStringLiteral(", ")))
          << QStringLiteral("- Ads: %1%2.").arg(int(number(stats, "ads")))
                 .arg(spend.isEmpty() ? QString() : QStringLiteral(", spent ") + spend.join(QStringLiteral(" + ")))
          << (results.isEmpty() ? QStringLiteral("- Analytics: no measured visits yet.")
              : QStringLiteral("- Analytics, last %1 days: %2 visits%3.").arg(m_rangeDays).arg(fullCount(number(results, "sessions")))
                    .arg(results.contains(QStringLiteral("conversions"))
                             ? QStringLiteral(", %1 conversions").arg(fullCount(number(results, "conversions"))) : QString()))
          << QString() << QStringLiteral("## Checklist") << QString();
    const QJsonArray tasks = item.value(QStringLiteral("tasks")).toArray();
    if (tasks.isEmpty())
        lines << QStringLiteral("- (empty)");
    for (const QJsonValue &value : tasks)
        lines << QStringLiteral("- [%1] %2").arg(value.toObject().value(QStringLiteral("done")).toBool() ? QStringLiteral("x") : QStringLiteral(" "),
                                                 text(value.toObject(), "title"));
    if (!text(item, "brief").isEmpty())
        lines << QString() << QStringLiteral("## Notes") << QString() << item.value(QStringLiteral("brief")).toString();
    return lines.join(QLatin1Char('\n'));
}

QString MarketingFeature::channelContext(const QString &id) const
{
    QJsonObject item;
    for (const QJsonValue &value : m_summaryData.value(QStringLiteral("channels")).toArray()) {
        if (text(value.toObject(), "channel") == id)
            item = value.toObject();
    }
    QJsonObject settings;
    for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray()) {
        if (text(value.toObject(), "id") == id)
            settings = value.toObject();
    }
    const QJsonObject health = item.value(QStringLiteral("health")).toObject();
    const QJsonObject feed = settings.value(QStringLiteral("feed")).toObject();
    const QString name = channelName(id, m_settings, m_channelCatalog);
    const QString category = channelCategory(id, m_channelCatalog);
    const QString organization = m_organization->currentData().toString();
    const QList<QJsonObject> history = seriesByChannel(m_metrics).value(id);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 start = now - m_rangeDays * kDay;
    const int cadence = qMax(1, int(number(health, "cadenceDays")));
    const Headline main = headline(id, item, health);
    const auto day = [](qint64 at) {
        return at > 0 ? QDateTime::fromMSecsSinceEpoch(at).toString(QStringLiteral("yyyy-MM-dd"))
                      : QStringLiteral("never");
    };
    const auto cell = [](QString value) {
        value = value.isEmpty() ? kDash : value;
        return value.replace(QLatin1Char('|'), QStringLiteral("\\|")).replace(QLatin1Char('\n'), QLatin1Char(' '));
    };
    const auto clip = [](const QString &value, int size) {
        const QString flat = value.simplified();
        return flat.size() > size ? flat.left(size - 1).trimmed() + QChar(0x2026) : flat;
    };
    const auto count = [](const QJsonValue &value) { return value.isDouble() ? fullCount(value.toDouble()) : kDash; };

    QStringList lines{
        QStringLiteral("# Grow %1 for %2").arg(name, organization), QString(),
        QStringLiteral("You are the marketing analyst for the %1 channel of %2. Below is everything the ForkMesh "
                       "Marketing dashboard knows about the channel on %3. Work from these numbers, and say when they "
                       "are too thin to support a claim.").arg(name, organization, day(now)),
        QString(), QStringLiteral("## What to do"), QString(),
        QStringLiteral("1. In two sentences, say how the channel is doing against its goal and its aim of a post every %1.")
            .arg(cadence == 1 ? QStringLiteral("day") : QStringLiteral("%1 days").arg(cadence)),
        QStringLiteral("2. Find what worked: match the posts and content below to the changes in the numbers after them."),
        QStringLiteral("3. Give the three changes most likely to raise %1 in the next 14 days. For each: what to do, "
                       "the data behind it, the number to watch, and the result you expect by when.").arg(main.label),
        QStringLiteral("4. Draft the next three posts for this channel, ready to publish."),
        QStringLiteral("5. Learn from last time: if this chat already holds an earlier copy of this context, or "
                       "\"Since the last copy\" below has numbers, check your earlier predictions against them, say "
                       "which held and which did not, and change your advice to match."),
        QStringLiteral("6. End with \"Notes for next time\": your predictions with the numbers to check, the data "
                       "missing here that would change your advice, and a better version of this \"What to do\" list "
                       "for the next copy."),
        QString(), QStringLiteral("## Channel"), QString(), QStringLiteral("| Field | Value |"), QStringLiteral("|---|---|"),
        QStringLiteral("| Channel | %1 |").arg(cell(name)),
        QStringLiteral("| Category | %1 |").arg(cell(category)),
        QStringLiteral("| Link | %1 |").arg(cell(channelOpenUrl(settings))),
        QStringLiteral("| About | %1 |").arg(cell(clip(text(feed, "description"), 400))),
        QStringLiteral("| Status | %1 |").arg(settings.value(QStringLiteral("enabled")).toBool(true)
                                                   ? QStringLiteral("Active") : QStringLiteral("Paused")),
        QStringLiteral("| Health | %1 |").arg(cell(healthLabel(text(health, "state")))),
        QStringLiteral("| Posting aim | every %1 day%2 |").arg(cadence).arg(cadence == 1 ? QString() : QStringLiteral("s")),
        QStringLiteral("| Posts in the last 30 days | %1 |").arg(fullCount(number(health, "postsLast30d"))),
        QStringLiteral("| Last post | %1 |").arg(day(qint64(number(health, "lastPostAt")))),
        QStringLiteral("| Engagement rate | %1 |").arg(cell(health.value(QStringLiteral("engagementRate")).isDouble()
            ? QStringLiteral("%1% (%2)").arg(QString::number(number(health, "engagementRate"), 'f', 1),
                                             engagementFormula(health))
            : engagementFormula(health))),
        QStringLiteral("| Numbers last read | %1 |").arg(cell(history.isEmpty() ? QStringLiteral("never")
            : QStringLiteral("%1, %2").arg(day(qint64(number(history.constLast(), "observedAt"))),
                                           sourceLabel(text(history.constLast(), "source")))))};
    if (!text(feed, "lastError").isEmpty())
        lines << QStringLiteral("| Last refresh failed | %1 |")
                     .arg(cell(refreshErrorText(id, text(feed, "lastError"), text(feed, "lastError"))));

    QList<int> shown;
    for (int index = 0; index < int(std::size(kContextKeys)); ++index) {
        const char *key = kContextKeys[index];
        if (number(item, key) > 0 || std::any_of(history.cbegin(), history.cend(),
                [key](const QJsonObject &snapshot) { return number(snapshot, key) > 0; }))
            shown << index;
    }
    const QJsonObject before = snapshotAt(history, start);
    lines << QString() << QStringLiteral("## Numbers") << QString();
    if (shown.isEmpty()) {
        lines << QStringLiteral("No numbers recorded yet.");
    } else {
        lines << QStringLiteral("| Metric | Now | %1 days ago |").arg(m_rangeDays) << QStringLiteral("|---|---|---|");
        for (int index : std::as_const(shown)) {
            const char *key = kContextKeys[index];
            lines << QStringLiteral("| %1 | %2 | %3 |").arg(QLatin1String(kContextLabels[index]), count(item.value(QLatin1String(key))),
                                                         before.isEmpty() ? kDash : count(before.value(QLatin1String(key))));
        }
    }

    lines << QString() << QStringLiteral("## Since the last copy") << QString();
    const QVariantMap last = QSettings().value(contextKey(organization, id)).toMap();
    const qint64 lastAt = last.value(QStringLiteral("at")).toLongLong();
    if (lastAt <= 0) {
        lines << QStringLiteral("This is the first copy of this context, so there is nothing to compare yet.");
    } else {
        const QVariantMap then = last.value(QStringLiteral("values")).toMap();
        const QVariantMap current = contextNumbers(item);
        lines << QStringLiteral("Last copied on %1 (%2). Then → now:").arg(day(lastAt), relativeLabel(lastAt));
        const auto change = [&lines, &then, &current](const QString &key, const QString &label) {
            if (!then.contains(key) && !current.contains(key))
                return;
            const double was = then.value(key).toDouble();
            const double is = current.value(key).toDouble();
            lines << QStringLiteral("- %1: %2 → %3 (%4%5)").arg(label, fullCount(was), fullCount(is),
                                                               is >= was ? QStringLiteral("+") : QStringLiteral("−"),
                                                               fullCount(std::abs(is - was)));
        };
        for (int index = 0; index < int(std::size(kContextKeys)); ++index)
            change(QLatin1String(kContextKeys[index]), QLatin1String(kContextLabels[index]));
        change(QStringLiteral("postsLast30d"), QStringLiteral("Posts in the last 30 days"));
    }

    QList<QJsonObject> recent;
    for (const QJsonObject &snapshot : history) {
        if (number(snapshot, "observedAt") > start)
            recent.prepend(snapshot);
    }
    if (!recent.isEmpty() && !shown.isEmpty()) {
        lines << QString() << QStringLiteral("## History, newest first") << QString();
        QStringList head{QStringLiteral("Date")};
        QStringList rule{QStringLiteral("---")};
        for (int index : std::as_const(shown)) {
            head << QLatin1String(kContextLabels[index]);
            rule << QStringLiteral("---");
        }
        head << QStringLiteral("Source") << QStringLiteral("Note");
        rule << QStringLiteral("---") << QStringLiteral("---");
        lines << QStringLiteral("| %1 |").arg(head.join(QStringLiteral(" | ")))
              << QStringLiteral("|%1|").arg(rule.join(QLatin1Char('|')));
        for (const QJsonObject &snapshot : recent.mid(0, 40)) {
            QStringList row{day(qint64(number(snapshot, "observedAt")))};
            for (int index : std::as_const(shown))
                row << count(snapshot.value(QLatin1String(kContextKeys[index])));
            row << cell(sourceLabel(text(snapshot, "source"))) << cell(clip(text(snapshot, "note"), 160));
            lines << QStringLiteral("| %1 |").arg(row.join(QStringLiteral(" | ")));
        }
    }

    const QJsonObject traffic = channelViews(id);
    if (!traffic.isEmpty()) {
        QStringList perDay;
        for (double views : dailyViews(traffic.value(QStringLiteral("days")).toArray(), start, now, m_rangeDays))
            perDay << fullCount(views);
        lines << QString() << QStringLiteral("## Site views from Analytics") << QString()
              << QStringLiteral("%1 views from %2 visitors on %3%4 in the last %5 days.")
                     .arg(fullCount(number(traffic, "views")), fullCount(number(traffic, "visitors")), text(traffic, "host"),
                          text(traffic, "path") == QLatin1String("/") ? QString() : text(traffic, "path"))
                     .arg(m_rangeDays)
              << QStringLiteral("Per day, oldest first: %1").arg(perDay.join(QStringLiteral(", ")));
    }

    QStringList posted;
    for (const QJsonValue &value : std::as_const(m_posts)) {
        const QJsonObject post = value.toObject();
        if (text(post, "channel") != id || posted.size() >= 30)
            continue;
        posted << QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 |")
                      .arg(day(qint64(number(post, "postedAt"))),
                           cell(clip(text(post, "title").isEmpty() ? text(post, "url") : text(post, "title"), 160)),
                           count(post.value(QStringLiteral("views"))), count(post.value(QStringLiteral("likes"))),
                           count(post.value(QStringLiteral("replies"))), count(post.value(QStringLiteral("reposts"))));
    }
    if (!posted.isEmpty())
        lines << QString() << QStringLiteral("## Posted links and their stats") << QString()
              << QStringLiteral("| Posted | Post | Views | Likes | Replies | Reposts |")
              << QStringLiteral("|---|---|---|---|---|---|") << posted;

    QList<QJsonObject> content;
    for (const QJsonValue &value : std::as_const(m_content)) {
        if (text(value.toObject(), "channel") == id && text(value.toObject(), "status") != QLatin1String("archived"))
            content.append(value.toObject());
    }
    const auto when = [](const QJsonObject &entry) {
        for (const char *key : {"publishedAt", "scheduledAt", "updatedAt"}) {
            if (number(entry, key) > 0)
                return qint64(number(entry, key));
        }
        return qint64(0);
    };
    std::stable_sort(content.begin(), content.end(), [&when](const QJsonObject &a, const QJsonObject &b) {
        return when(a) > when(b);
    });
    if (!content.isEmpty()) {
        lines << QString() << QStringLiteral("## Content for this channel, newest first") << QString()
              << QStringLiteral("| Date | Status | Title | Text |") << QStringLiteral("|---|---|---|---|");
        for (const QJsonObject &entry : content.mid(0, 25))
            lines << QStringLiteral("| %1 | %2 | %3 | %4 |").arg(day(when(entry)), cell(text(entry, "status")),
                                                                cell(clip(text(entry, "title"), 120)),
                                                                cell(clip(text(entry, "body"), 280)));
    }

    if (id == QLatin1String("discord") && !m_discordPosts.isEmpty()) {
        const QJsonArray posts = m_discordPosts.value(QStringLiteral("posts")).toArray();
        QStringList channels;
        for (const QJsonValue &value : m_discordPosts.value(QStringLiteral("channels")).toArray())
            channels << QStringLiteral("#") + value.toString();
        lines << QString()
              << QStringLiteral("## Discord posts in the last %1 days").arg(int(number(m_discordPosts, "days")))
              << QString()
              << QStringLiteral("%1 post%2 in %3, read %4.").arg(posts.size())
                     .arg(posts.size() == 1 ? QString() : QStringLiteral("s"), channels.join(QStringLiteral(", ")),
                          relativeLabel(m_discordPostsAt));
        if (m_discordPosts.value(QStringLiteral("truncated")).toBool())
            lines << QStringLiteral("Only the latest 50 posts of each channel were read.");
        if (m_discordPosts.value(QStringLiteral("contentMissing")).toBool())
            lines << QStringLiteral("The bot cannot read message text, so the posts below have no text.");
        lines << QString();
        for (int index = 0; index < posts.size() && index < 60; ++index) {
            const QJsonObject post = posts.at(index).toObject();
            lines << QStringLiteral("- %1 · #%2 · %3%4: %5")
                         .arg(timestamp(qint64(number(post, "postedAt"))), text(post, "channel"), text(post, "author"),
                              post.value(QStringLiteral("bot")).toBool() ? QStringLiteral(" (bot)") : QString(),
                              text(post, "content").isEmpty() ? QStringLiteral("(no text)")
                                                              : clip(text(post, "content"), 300));
        }
    }

    QStringList ads;
    for (const QJsonValue &value : std::as_const(m_ads)) {
        const QJsonObject ad = value.toObject();
        if (text(ad, "channel") == id)
            ads << QStringLiteral("- %1: %2, spent %3").arg(text(ad, "name"), adLabel(text(ad, "status")),
                                                            money(ad.value(QStringLiteral("spendCents")), text(ad, "currency")));
    }
    if (!ads.isEmpty())
        lines << QString() << QStringLiteral("## Ads") << QString() << ads;

    QStringList campaigns;
    for (const QJsonValue &value : std::as_const(m_campaigns)) {
        const QJsonObject campaign = value.toObject();
        if (!campaign.value(QStringLiteral("channels")).toArray().contains(id) ||
            text(campaign, "status") == QLatin1String("archived"))
            continue;
        campaigns << QStringLiteral("- %1 (%2, %3 to %4): %5").arg(text(campaign, "name"), text(campaign, "status"),
                                                                   day(qint64(number(campaign, "startsAt"))),
                                                                   day(qint64(number(campaign, "endsAt"))),
                                                                   clip(text(campaign, "message").isEmpty()
                                                                            ? text(campaign, "objective")
                                                                            : text(campaign, "message"), 240));
    }
    if (!campaigns.isEmpty())
        lines << QString() << QStringLiteral("## Campaigns on this channel") << QString() << campaigns;

    const QJsonArray checks = health.value(QStringLiteral("checks")).toArray();
    if (!checks.isEmpty()) {
        lines << QString() << QStringLiteral("## Health checks") << QString();
        for (const QJsonValue &value : checks)
            lines << QStringLiteral("- [%1] %2").arg(value.toObject().value(QStringLiteral("ok")).toBool()
                                                         ? QStringLiteral("x") : QStringLiteral(" "),
                                                     text(value.toObject(), "label"));
    }

    const QJsonObject playbook = m_playbooks.value(category).toObject();
    if (!playbook.isEmpty()) {
        QStringList kpis;
        for (const QJsonValue &kpi : playbook.value(QStringLiteral("kpis")).toArray())
            kpis << kpi.toString();
        lines << QString() << QStringLiteral("## Playbook for %1 channels").arg(category) << QString()
              << QStringLiteral("- Goal: %1").arg(text(playbook, "goal"));
        if (!kpis.isEmpty())
            lines << QStringLiteral("- Track: %1").arg(kpis.join(QStringLiteral(", ")));
        for (const QJsonValue &tip : playbook.value(QStringLiteral("tips")).toArray())
            lines << QStringLiteral("- Tip: %1").arg(tip.toString());
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

void MarketingFeature::copyChannelContext(const QString &id)
{
    const QString organization = m_organization->currentData().toString();
    if (organization.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(channelContext(id));
    QJsonObject item;
    for (const QJsonValue &value : m_summaryData.value(QStringLiteral("channels")).toArray()) {
        if (text(value.toObject(), "channel") == id)
            item = value.toObject();
    }
    QSettings().setValue(contextKey(organization, id),
                         QVariantMap{{QStringLiteral("at"), QDateTime::currentMSecsSinceEpoch()},
                                     {QStringLiteral("values"), contextNumbers(item)}});
    setStatus(QStringLiteral("Copied the AI context for %1. Paste it into Claude or another AI chat, and copy it "
                             "again after you act on the advice so the AI can check what worked.")
                  .arg(channelName(id, m_settings, m_channelCatalog)));
}

void MarketingFeature::editCampaign(const QJsonObject &campaign, const QJsonObject &preset)
{
    const QJsonObject source = campaign.isEmpty() ? preset : campaign;
    const QString id = text(campaign, "id");
    QDialog dialog(this);
    dialog.setWindowTitle(!id.isEmpty() ? QStringLiteral("Edit campaign")
                          : !text(preset, "label").isEmpty() ? QStringLiteral("New campaign · ") + text(preset, "label")
                                                             : QStringLiteral("New campaign"));
    dialog.resize(660, 760);
    auto *layout = new QVBoxLayout(&dialog);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    scroll->setWidget(page);
    layout->addWidget(scroll, 1);
    const auto joined = [&source](const char *key, const QString &separator) {
        QStringList parts;
        for (const QJsonValue &value : source.value(QLatin1String(key)).toArray())
            parts << value.toString();
        return parts.join(separator);
    };
    auto *name = lineEdit(text(source, "name"));
    auto *status = new QComboBox;
    for (const auto &choice : campaignStatuses())
        status->addItem(choice.second, choice.first);
    status->setCurrentIndex(qMax(0, status->findData(text(source, "status").isEmpty() ? QStringLiteral("planned")
                                                                                     : text(source, "status"))));
    auto *objective = new QComboBox;
    for (const auto &choice : campaignObjectives())
        objective->addItem(choice.second, choice.first);
    objective->setCurrentIndex(qMax(0, objective->findData(text(source, "objective"))));
    auto *owner = lineEdit(text(source, "owner"));
    const QDate unset(2000, 1, 1);
    const auto dateEdit = [&unset](double at) {
        auto *edit = new QDateEdit;
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        edit->setMinimumDate(unset);
        edit->setSpecialValueText(QStringLiteral("Not set"));
        edit->setDate(at > 0 ? QDateTime::fromMSecsSinceEpoch(qint64(at)).date() : unset);
        return edit;
    };
    auto *starts = dateEdit(number(source, "startsAt"));
    auto *ends = dateEdit(number(source, "endsAt"));
    auto *landing = lineEdit(text(source, "landingUrl"));
    landing->setPlaceholderText(QStringLiteral("https://"));
    auto *audience = lineEdit(text(source, "audience"));
    auto *message = new QPlainTextEdit(source.value(QStringLiteral("message")).toString());
    message->setFixedHeight(70);
    auto *offer = lineEdit(text(source, "offer"));
    const auto countEdit = [&source](const char *key) {
        auto *edit = lineEdit(source.value(QLatin1String(key)).isDouble()
                                  ? QString::number(qint64(number(source, key))) : QString());
        edit->setValidator(new QIntValidator(0, 2000000000, edit));
        edit->setPlaceholderText(QStringLiteral("No target"));
        return edit;
    };
    auto *targetVisits = countEdit("targetVisits");
    auto *targetConversions = countEdit("targetConversions");
    auto *budget = new QDoubleSpinBox;
    budget->setRange(0, 100000000);
    budget->setDecimals(2);
    budget->setValue(number(source, "budgetCents") / 100.0);
    auto *currency = lineEdit(text(source, "currency").isEmpty() ? QStringLiteral("USD") : text(source, "currency"));
    currency->setMaxLength(3);
    auto *budgetRow = new QHBoxLayout;
    budgetRow->addWidget(budget, 1);
    budgetRow->addWidget(currency);
    QStringList selected;
    for (const QJsonValue &value : source.value(QStringLiteral("channels")).toArray())
        selected << value.toString();
    QStringList ids;
    for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray())
        ids << text(value.toObject(), "id");
    for (const QString &channel : std::as_const(selected)) {
        if (!ids.contains(channel))
            ids << channel;
    }
    auto *channelBox = new QWidget;
    auto *channelGrid = new QGridLayout(channelBox);
    channelGrid->setContentsMargins(0, 0, 0, 0);
    QList<QCheckBox *> channelChecks;
    for (int index = 0; index < ids.size(); ++index) {
        auto *check = new QCheckBox(channelName(ids.at(index), m_settings, m_channelCatalog));
        check->setProperty("channel", ids.at(index));
        check->setChecked(selected.contains(ids.at(index)));
        channelGrid->addWidget(check, index / 3, index % 3);
        channelChecks << check;
    }
    auto *hashtags = lineEdit(joined("hashtags", QStringLiteral(" ")));
    auto *keywords = lineEdit(joined("keywords", QStringLiteral(", ")));
    QStringList taskLines;
    for (const QJsonValue &value : source.value(QStringLiteral("tasks")).toArray()) {
        const QJsonObject task = value.isObject() ? value.toObject() : QJsonObject{{QStringLiteral("title"), value.toString()}};
        taskLines << (task.value(QStringLiteral("done")).toBool() ? QStringLiteral("[x] ") : QStringLiteral("[ ] ")) + text(task, "title");
    }
    auto *tasks = new QPlainTextEdit(taskLines.join(QLatin1Char('\n')));
    tasks->setPlaceholderText(QStringLiteral("One task per line; start a line with [x] when it is done"));
    tasks->setFixedHeight(90);
    auto *brief = new QPlainTextEdit(source.value(QStringLiteral("brief")).toString());
    brief->setFixedHeight(90);
    auto *notes = new QPlainTextEdit(source.value(QStringLiteral("notes")).toString());
    notes->setFixedHeight(60);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Status"), status);
    form->addRow(QStringLiteral("Objective"), objective);
    form->addRow(QStringLiteral("Owner"), owner);
    form->addRow(QStringLiteral("Starts"), starts);
    form->addRow(QStringLiteral("Ends"), ends);
    form->addRow(QStringLiteral("Landing page"), landing);
    form->addRow(QStringLiteral("Audience"), audience);
    form->addRow(QStringLiteral("Message"), message);
    form->addRow(QStringLiteral("Offer"), offer);
    form->addRow(QStringLiteral("Target visits"), targetVisits);
    form->addRow(QStringLiteral("Target conversions"), targetConversions);
    form->addRow(QStringLiteral("Budget"), budgetRow);
    form->addRow(QStringLiteral("Channels"), channelBox);
    form->addRow(QStringLiteral("Hashtags"), hashtags);
    form->addRow(QStringLiteral("Search keywords"), keywords);
    form->addRow(QStringLiteral("Checklist"), tasks);
    form->addRow(QStringLiteral("Brief"), brief);
    form->addRow(QStringLiteral("Notes"), notes);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    bool remove = false;
    if (!id.isEmpty()) {
        auto *deleteButton = buttons->addButton(QStringLiteral("Delete"), QDialogButtonBox::DestructiveRole);
        deleteButton->setObjectName(QStringLiteral("dangerButton"));
        connect(deleteButton, &QPushButton::clicked, &dialog, [&dialog, &remove, &campaign] {
            if (QMessageBox::question(&dialog, QStringLiteral("Delete campaign"),
                                      QStringLiteral("Delete %1? Its content and ads stay.").arg(text(campaign, "name")))
                == QMessageBox::Yes) {
                remove = true;
                dialog.accept();
            }
        });
    }
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (remove) {
        request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/campaigns/") + id), {},
                [this](bool ok, const QJsonObject &, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("The campaign was not deleted: %1").arg(error), true);
            setStatus(QStringLiteral("Campaign deleted."));
            loadOrganization();
        });
        return;
    }
    const auto stamp = [&unset](QDateEdit *edit, const QTime &at) {
        return edit->date() <= unset ? 0.0 : double(QDateTime(edit->date(), at).toMSecsSinceEpoch());
    };
    const auto optionalCount = [](QLineEdit *edit) {
        return edit->text().trimmed().isEmpty() ? QJsonValue() : QJsonValue(edit->text().toLongLong());
    };
    QJsonArray channels;
    for (QCheckBox *check : std::as_const(channelChecks)) {
        if (check->isChecked())
            channels.append(check->property("channel").toString());
    }
    QJsonArray taskList;
    static const QRegularExpression done(QStringLiteral("^\\[[xX]\\]\\s*"));
    static const QRegularExpression box(QStringLiteral("^\\[[ xX]?\\]\\s*"));
    for (const QString &line : tasks->toPlainText().split(QLatin1Char('\n'))) {
        QString title = line.trimmed();
        if (title.isEmpty())
            continue;
        const bool finished = done.match(title).hasMatch();
        title.remove(box);
        taskList.append(QJsonObject{{QStringLiteral("title"), title}, {QStringLiteral("done"), finished}});
    }
    const QJsonObject body{
        {QStringLiteral("name"), name->text()},
        {QStringLiteral("status"), status->currentData().toString()},
        {QStringLiteral("objective"), objective->currentData().toString()},
        {QStringLiteral("owner"), owner->text()},
        {QStringLiteral("landingUrl"), landing->text()},
        {QStringLiteral("audience"), audience->text()},
        {QStringLiteral("message"), message->toPlainText()},
        {QStringLiteral("offer"), offer->text()},
        {QStringLiteral("brief"), brief->toPlainText()},
        {QStringLiteral("notes"), notes->toPlainText()},
        {QStringLiteral("startsAt"), stamp(starts, QTime(0, 0))},
        {QStringLiteral("endsAt"), stamp(ends, QTime(23, 59))},
        {QStringLiteral("hashtags"), hashtags->text()},
        {QStringLiteral("keywords"), keywords->text()},
        {QStringLiteral("budgetCents"), double(std::llround(budget->value() * 100))},
        {QStringLiteral("currency"), currency->text().trimmed().toUpper()},
        {QStringLiteral("targetVisits"), optionalCount(targetVisits)},
        {QStringLiteral("targetConversions"), optionalCount(targetConversions)},
        {QStringLiteral("channels"), channels},
        {QStringLiteral("tasks"), taskList},
        {QStringLiteral("source"), text(source, "source")},
    };
    request(id.isEmpty() ? QByteArrayLiteral("POST") : QByteArrayLiteral("PATCH"),
            organizationPath(QStringLiteral("/campaigns") + (id.isEmpty() ? QString() : QLatin1Char('/') + id)),
            body, [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The campaign was not saved: %1")
                                 .arg(text(payload, "error") == QLatin1String("campaign_name_required")
                                          ? QStringLiteral("give it a name") : error), true);
        setStatus(QStringLiteral("Campaign saved."));
        loadOrganization();
    });
}

void MarketingFeature::toggleCampaignTask(const QString &id, int index, bool done)
{
    const QJsonObject campaign = campaignById(id);
    QJsonArray tasks = campaign.value(QStringLiteral("tasks")).toArray();
    if (index < 0 || index >= tasks.size())
        return;
    QJsonObject task = tasks.at(index).toObject();
    task.insert(QStringLiteral("done"), done);
    tasks.replace(index, task);
    request(QByteArrayLiteral("PATCH"), organizationPath(QStringLiteral("/campaigns/") + id),
            QJsonObject{{QStringLiteral("tasks"), tasks}},
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("The checklist was not updated: %1").arg(error), true);
            return renderCampaigns();
        }
        const QJsonObject updated = payload.value(QStringLiteral("campaign")).toObject();
        for (int row = 0; row < m_campaigns.size(); ++row) {
            if (text(m_campaigns.at(row).toObject(), "id") == id && !updated.isEmpty())
                m_campaigns.replace(row, updated);
        }
        QTimer::singleShot(0, this, [this] { renderCampaigns(); });
    });
}

void MarketingFeature::renderAssistantCampaigns()
{
    const QString previous = m_assistantCampaign->currentData().toString();
    const QSignalBlocker blocker(m_assistantCampaign);
    m_assistantCampaign->clear();
    m_assistantCampaign->addItem(QStringLiteral("All marketing"), QString());
    for (const QJsonValue &value : std::as_const(m_campaigns)) {
        const QJsonObject campaign = value.toObject();
        if (isOpenCampaign(text(campaign, "status")))
            m_assistantCampaign->addItem(text(campaign, "name"), text(campaign, "id"));
    }
    m_assistantCampaign->setCurrentIndex(qMax(0, m_assistantCampaign->findData(previous)));
    m_assistantForm->setVisible(m_assistantAvailable);
}

void MarketingFeature::renderAttention()
{
    const DashboardColors colors = DashboardColors::from(this);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    struct Item {
        int rank = 0;
        QString level;
        QString title;
        QString detail;
        QString verb;
        std::function<void()> run;
    };
    QList<Item> items;
    static const QHash<QString, int> ranks{
        {QStringLiteral("warning"), 0}, {QStringLiteral("action"), 1},
        {QStringLiteral("setup"), 2}, {QStringLiteral("info"), 3}};
    static const QHash<QString, QString> verbs{
        {QStringLiteral("campaign"), QStringLiteral("New")}, {QStringLiteral("campaign-edit"), QStringLiteral("Open")},
        {QStringLiteral("content"), QStringLiteral("Draft")}, {QStringLiteral("channel"), QStringLiteral("View")},
        {QStringLiteral("keyword"), QStringLiteral("Add")}};
    for (int index = 0; index < m_suggestions.size(); ++index) {
        const QJsonObject item = m_suggestions.at(index).toObject();
        const QString level = text(item, "level");
        const QString type = text(item.value(QStringLiteral("action")).toObject(), "type");
        items.append({ranks.value(level, 3), level, text(item, "title"), text(item, "detail"),
                      type.isEmpty() ? QString() : verbs.value(type, QStringLiteral("Open")),
                      [this, index] { runSuggestion(index); }});
    }
    for (const AdAttention &entry : adAttention()) {
        const QJsonObject ad = entry.ad;
        items.append({entry.urgent ? 0 : 1, entry.urgent ? QStringLiteral("warning") : QStringLiteral("action"),
                      QStringLiteral("%1: %2").arg(entry.label, text(ad, "name")),
                      channelLabel(text(ad, "channel")) + QStringLiteral(" · ") + entry.detail,
                      QStringLiteral("Open"), [this, ad] { editAd(ad); }});
    }
    for (const QJsonValue &value : std::as_const(m_content)) {
        const QJsonObject item = value.toObject();
        const QString status = text(item, "status");
        const qint64 scheduledAt = qint64(number(item, "scheduledAt"));
        const qint64 updatedAt = qint64(number(item, "updatedAt"));
        const QString where = channelName(text(item, "channel"), m_settings, m_channelCatalog);
        if ((status == QLatin1String("queued") || status == QLatin1String("scheduled")) &&
            scheduledAt > 0 && scheduledAt < now)
            items.append({0, QStringLiteral("warning"), QStringLiteral("“%1” missed its slot").arg(text(item, "title")),
                          QStringLiteral("%1 · scheduled %2 and not marked published").arg(where, relativeLabel(scheduledAt)),
                          QStringLiteral("Open"), [this, item] { editContent(item); }});
        else if (status == QLatin1String("review") && updatedAt > 0 && now - updatedAt > 3 * kDay)
            items.append({1, QStringLiteral("action"),
                          QStringLiteral("“%1” in review for %2 days").arg(text(item, "title")).arg((now - updatedAt) / kDay),
                          text(item, "owner").isEmpty() ? where : QStringLiteral("%1 · %2").arg(where, text(item, "owner")),
                          QStringLiteral("Open"), [this, item] { editContent(item); }});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.rank < b.rank; });

    const auto tile = [this, &colors](const QString &level, const QString &title, const QString &detail,
                                      const QString &verb, const std::function<void()> &run) {
        auto *frame = new QFrame;
        frame->setObjectName(QStringLiteral("marketingIdea"));
        auto *layout = new QHBoxLayout(frame);
        layout->setContentsMargins(8, 4, 6, 4);
        layout->setSpacing(6);
        const bool primary = level == QLatin1String("setup") || level == QLatin1String("action");
        const QString glyph = level == QLatin1String("warning") ? QStringLiteral("⚠")
            : level == QLatin1String("win") ? QStringLiteral("✓")
            : level == QLatin1String("setup") ? QStringLiteral("✎")
            : level == QLatin1String("action") ? QStringLiteral("◷") : QStringLiteral("•");
        const QColor tone = level == QLatin1String("warning") ? colors.warn
            : level == QLatin1String("win") ? colors.good : primary ? colors.primary : colors.muted;
        auto *icon = new QLabel(glyph);
        icon->setStyleSheet(QStringLiteral("color: %1;").arg(tone.name()));
        icon->setFixedWidth(14);
        layout->addWidget(icon, 0, Qt::AlignTop);
        const QString shortDetail = detail.size() > 140 ? detail.left(137) + QStringLiteral("…") : detail;
        auto *body = new QLabel(QStringLiteral("<b>%1</b>").arg(escaped(title)) + (shortDetail.isEmpty() ? QString()
            : QStringLiteral("<br><span style=\"color:%1; font-size:11px\">%2</span>").arg(colors.muted.name(), escaped(shortDetail))));
        body->setObjectName(QStringLiteral("marketingNote"));
        body->setTextFormat(Qt::RichText);
        body->setWordWrap(true);
        if (shortDetail != detail)
            body->setToolTip(detail);
        layout->addWidget(body, 1);
        if (!verb.isEmpty()) {
            auto *action = linkButton(verb);
            connect(action, &QPushButton::clicked, this, run);
            layout->addWidget(action, 0, Qt::AlignTop);
        }
        return frame;
    };

    constexpr int kCollapsed = 6;
    const int urgent = int(std::count_if(items.cbegin(), items.cend(), [](const Item &item) { return item.rank == 0; }));
    QList<QWidget *> tiles;
    if (items.isEmpty())
        tiles << tile(QStringLiteral("win"), QStringLiteral("Nothing needs attention."),
                      m_assistantAvailable ? QStringLiteral("Ask the Marketing agent for ideas.") : QString(), QString(), {});
    const int shown = m_attentionAll ? int(items.size()) : qMin(int(items.size()), kCollapsed);
    for (int index = 0; index < shown; ++index) {
        const Item &item = items.at(index);
        tiles << tile(item.level, item.title, item.detail, item.verb, item.run);
    }
    m_attentionGrid->setTiles(tiles);
    m_attentionPanel->hint()->setText(items.isEmpty() ? QString()
        : urgent > 0 ? QStringLiteral("%1 urgent · %2 to review").arg(urgent).arg(items.size())
                     : QStringLiteral("%1 to review").arg(items.size()));
    m_attentionMore->setText(m_attentionAll ? QStringLiteral("Show fewer")
                                            : QStringLiteral("Show all %1").arg(items.size()));
    m_attentionMore->setVisible(items.size() > kCollapsed);
}

void MarketingFeature::runSuggestion(int index)
{
    const QJsonObject action = m_suggestions.at(index).toObject().value(QStringLiteral("action")).toObject();
    const QString type = text(action, "type");
    if (type == QLatin1String("campaign")) {
        editCampaign(QJsonObject());
    } else if (type == QLatin1String("campaign-edit")) {
        editCampaign(campaignById(text(action, "id")));
    } else if (type == QLatin1String("content")) {
        editContent(QJsonObject{{QStringLiteral("campaign"), text(action, "campaign")},
                                {QStringLiteral("channel"), text(action, "channel")},
                                {QStringLiteral("status"), QStringLiteral("draft")}});
    } else if (type == QLatin1String("channel")) {
        openChannel(text(action, "channel"));
    } else if (type == QLatin1String("keyword")) {
        editKeyword(QJsonObject());
    }
}

void MarketingFeature::runAssistant(const QString &campaignId)
{
    if (m_assistantBusy || !m_installed || !m_assistantAvailable)
        return;
    if (!campaignId.isNull())
        m_assistantCampaign->setCurrentIndex(qMax(0, m_assistantCampaign->findData(campaignId)));
    m_assistantBusy = true;
    m_assistantRun->setEnabled(false);
    m_assistantRun->setText(QStringLiteral("Thinking…"));
    renderAssistant();
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/assistant")),
            QJsonObject{{QStringLiteral("campaignId"), m_assistantCampaign->currentData().toString()},
                        {QStringLiteral("question"), m_assistantQuestion->text()}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_assistantBusy = false;
        m_assistantRun->setEnabled(true);
        m_assistantRun->setText(QStringLiteral("Suggest"));
        m_assistant = ok ? payload
            : QJsonObject{{QStringLiteral("error"), text(payload, "error") == QLatin1String("rate_limited")
                               ? QStringLiteral("The agent is busy; try again in a minute.")
                               : QStringLiteral("The agent could not answer: %1").arg(error)}};
        renderAssistant();
    });
}

void MarketingFeature::renderAssistant()
{
    const DashboardColors colors = DashboardColors::from(this);
    clearLayout(m_assistantBox);
    const auto note = [this](const QString &value, const char *objectName) {
        auto *label = new QLabel(value);
        label->setObjectName(QLatin1String(objectName));
        label->setWordWrap(true);
        m_assistantBox->addWidget(label);
    };
    if (m_assistantBusy)
        return note(QStringLiteral("The Marketing agent is reading your campaigns, channels and keywords…"), "marketingHint");
    if (m_assistant.isEmpty())
        return;
    if (!text(m_assistant, "error").isEmpty())
        return note(text(m_assistant, "error"), "marketingWarn");
    if (!text(m_assistant, "summary").isEmpty())
        note(text(m_assistant, "summary"), "marketingNote");
    const QJsonObject campaign = campaignById(text(m_assistant, "campaignId"));
    static const QHash<QString, QString> labels{
        {QStringLiteral("content"), QStringLiteral("Add draft")}, {QStringLiteral("task"), QStringLiteral("Add to checklist")},
        {QStringLiteral("keyword"), QStringLiteral("Track")}, {QStringLiteral("campaign"), QStringLiteral("Create")}};
    const QJsonArray ideas = m_assistant.value(QStringLiteral("suggestions")).toArray();
    for (int index = 0; index < ideas.size(); ++index) {
        const QJsonObject idea = ideas.at(index).toObject();
        const QString kind = text(idea, "kind");
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("marketingIdea"));
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(4);
        auto *head = new QHBoxLayout;
        auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(escaped(text(idea, "title"))));
        title->setObjectName(QStringLiteral("marketingNote"));
        title->setTextFormat(Qt::RichText);
        title->setWordWrap(true);
        head->addWidget(title, 1);
        if (idea.value(QStringLiteral("applied")).toBool()) {
            auto *added = new QLabel(QStringLiteral("✓ Added"));
            added->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(colors.good.name()));
            head->addWidget(added, 0, Qt::AlignTop);
        } else {
            auto *apply = linkButton(labels.value(kind, QStringLiteral("Add")));
            if (kind == QLatin1String("task") && campaign.isEmpty()) {
                apply->setEnabled(false);
                apply->setToolTip(QStringLiteral("Pick a campaign first"));
            }
            connect(apply, &QPushButton::clicked, this, [this, index] { applyIdea(index); });
            head->addWidget(apply, 0, Qt::AlignTop);
        }
        layout->addLayout(head);
        QStringList meta;
        if (!text(idea, "channel").isEmpty())
            meta << channelName(text(idea, "channel"), m_settings, m_channelCatalog);
        const int inDays = int(number(idea, "inDays"));
        if (inDays > 0)
            meta << QStringLiteral("in %1 day%2").arg(inDays).arg(inDays == 1 ? QString() : QStringLiteral("s"));
        else if (kind == QLatin1String("content"))
            meta << QStringLiteral("today");
        QStringList parts;
        if (!meta.isEmpty())
            parts << meta.join(QStringLiteral(" · "));
        if (!text(idea, "detail").isEmpty())
            parts << text(idea, "detail");
        if (!parts.isEmpty()) {
            auto *detail = new QLabel(parts.join(QStringLiteral(" — ")));
            detail->setObjectName(QStringLiteral("marketingHint"));
            detail->setWordWrap(true);
            layout->addWidget(detail);
        }
        if (!text(idea, "copy").isEmpty()) {
            auto *copy = new QLabel(idea.value(QStringLiteral("copy")).toString());
            copy->setObjectName(QStringLiteral("marketingQuote"));
            copy->setWordWrap(true);
            copy->setTextFormat(Qt::PlainText);
            copy->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(copy);
        }
        m_assistantBox->addWidget(card);
    }
    note(QStringLiteral("Suggestions are drafts: nothing is posted or spent until a person does it."), "marketingHint");
}

void MarketingFeature::applyIdea(int index)
{
    QJsonArray ideas = m_assistant.value(QStringLiteral("suggestions")).toArray();
    if (index < 0 || index >= ideas.size())
        return;
    QJsonObject idea = ideas.at(index).toObject();
    const QJsonObject campaign = campaignById(text(m_assistant, "campaignId"));
    const QString kind = text(idea, "kind");
    if (kind == QLatin1String("campaign")) {
        editCampaign(QJsonObject(), QJsonObject{{QStringLiteral("name"), text(idea, "title")},
                                                {QStringLiteral("brief"), text(idea, "detail")},
                                                {QStringLiteral("status"), QStringLiteral("idea")}});
        return;
    }
    QByteArray method = QByteArrayLiteral("POST");
    QString path;
    QJsonObject body;
    if (kind == QLatin1String("content")) {
        const int inDays = int(number(idea, "inDays"));
        path = organizationPath(QStringLiteral("/content"));
        body = QJsonObject{
            {QStringLiteral("title"), text(idea, "title")},
            {QStringLiteral("body"), text(idea, "copy").isEmpty() ? text(idea, "detail") : idea.value(QStringLiteral("copy")).toString()},
            {QStringLiteral("channel"), text(idea, "channel")},
            {QStringLiteral("campaign"), text(campaign, "name")},
            {QStringLiteral("status"), QStringLiteral("draft")},
            {QStringLiteral("scheduledAt"), inDays > 0 ? double(QDateTime(QDate::currentDate().addDays(inDays), QTime(10, 0)).toMSecsSinceEpoch()) : 0.0},
            {QStringLiteral("notes"), QStringLiteral("Suggested by the Marketing agent.")}};
    } else if (kind == QLatin1String("task") && !campaign.isEmpty()) {
        QJsonArray tasks = campaign.value(QStringLiteral("tasks")).toArray();
        tasks.append(QJsonObject{{QStringLiteral("title"), text(idea, "title")}, {QStringLiteral("done"), false}});
        method = QByteArrayLiteral("PATCH");
        path = organizationPath(QStringLiteral("/campaigns/") + text(campaign, "id"));
        body = QJsonObject{{QStringLiteral("tasks"), tasks}};
    } else if (kind == QLatin1String("keyword")) {
        path = organizationPath(QStringLiteral("/keywords"));
        body = QJsonObject{{QStringLiteral("term"), text(idea, "title")},
                           {QStringLiteral("campaign"), text(campaign, "name")},
                           {QStringLiteral("notes"), text(idea, "detail")}};
    } else {
        return;
    }
    request(method, path, body, [this, index](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(text(payload, "error") == QLatin1String("keyword_exists")
                                 ? QStringLiteral("That keyword is already tracked.")
                                 : QStringLiteral("Could not add it: %1").arg(error), true);
        QJsonArray current = m_assistant.value(QStringLiteral("suggestions")).toArray();
        if (index < current.size()) {
            QJsonObject applied = current.at(index).toObject();
            applied.insert(QStringLiteral("applied"), true);
            current.replace(index, applied);
            m_assistant.insert(QStringLiteral("suggestions"), current);
        }
        renderAssistant();
        loadOrganization();
    });
}

void MarketingFeature::editKeyword(const QJsonObject &keyword, const QString &term)
{
    const QString id = text(keyword, "id");
    QDialog dialog(this);
    dialog.setWindowTitle(id.isEmpty() ? QStringLiteral("Track a keyword") : text(keyword, "term"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *termEdit = lineEdit(id.isEmpty() ? term : text(keyword, "term"));
    auto *campaign = new QComboBox;
    campaign->setEditable(true);
    campaign->addItem(QString());
    for (const QJsonValue &value : std::as_const(m_campaigns))
        campaign->addItem(text(value.toObject(), "name"));
    campaign->setCurrentText(text(keyword, "campaign"));
    auto *priority = new QComboBox;
    priority->addItem(QStringLiteral("Normal"), 0);
    for (int stars = 1; stars <= 3; ++stars)
        priority->addItem(QString(stars, QChar(0x2605)), stars);
    priority->setCurrentIndex(qBound(0, int(number(keyword, "priority")), 3));
    auto *target = lineEdit(text(keyword, "targetUrl"));
    target->setPlaceholderText(QStringLiteral("https:// page that should rank"));
    auto *notes = new QPlainTextEdit(keyword.value(QStringLiteral("notes")).toString());
    notes->setFixedHeight(70);
    auto *position = lineEdit();
    position->setValidator(new QDoubleValidator(0, 1000, 1, position));
    auto *impressions = lineEdit();
    impressions->setValidator(new QIntValidator(0, 2000000000, impressions));
    auto *clicks = lineEdit();
    clicks->setValidator(new QIntValidator(0, 2000000000, clicks));
    for (QLineEdit *edit : {position, impressions, clicks})
        edit->setPlaceholderText(QStringLiteral("Today's reading (optional)"));
    form->addRow(QStringLiteral("Keyword"), termEdit);
    form->addRow(QStringLiteral("Campaign"), campaign);
    form->addRow(QStringLiteral("Priority"), priority);
    form->addRow(QStringLiteral("Target page"), target);
    form->addRow(QStringLiteral("Notes"), notes);
    form->addRow(QStringLiteral("Position"), position);
    form->addRow(QStringLiteral("Impressions"), impressions);
    form->addRow(QStringLiteral("Clicks"), clicks);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    bool remove = false;
    if (!id.isEmpty()) {
        auto *deleteButton = buttons->addButton(QStringLiteral("Stop tracking"), QDialogButtonBox::DestructiveRole);
        deleteButton->setObjectName(QStringLiteral("dangerButton"));
        connect(deleteButton, &QPushButton::clicked, &dialog, [&dialog, &remove] {
            remove = true;
            dialog.accept();
        });
    }
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto done = [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(text(payload, "error") == QLatin1String("keyword_exists")
                                 ? QStringLiteral("That keyword is already tracked.")
                                 : QStringLiteral("The keyword was not saved: %1").arg(error), true);
        loadOrganization();
    };
    if (remove) {
        request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/keywords/") + id), {}, done);
        return;
    }
    QJsonObject body{{QStringLiteral("term"), termEdit->text()},
                     {QStringLiteral("campaign"), campaign->currentText()},
                     {QStringLiteral("priority"), priority->currentData().toInt()},
                     {QStringLiteral("targetUrl"), target->text()},
                     {QStringLiteral("notes"), notes->toPlainText()}};
    if (!position->text().isEmpty() || !impressions->text().isEmpty() || !clicks->text().isEmpty())
        body.insert(QStringLiteral("snapshot"), QJsonObject{
            {QStringLiteral("position"), position->text()}, {QStringLiteral("impressions"), impressions->text()},
            {QStringLiteral("clicks"), clicks->text()},
            {QStringLiteral("at"), double(QDateTime::currentMSecsSinceEpoch())},
            {QStringLiteral("source"), QStringLiteral("manual")}});
    request(id.isEmpty() ? QByteArrayLiteral("POST") : QByteArrayLiteral("PATCH"),
            organizationPath(QStringLiteral("/keywords") + (id.isEmpty() ? QString() : QLatin1Char('/') + id)),
            body, done);
}

void MarketingFeature::renderFlow()
{
    const DashboardColors colors = DashboardColors::from(this);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QHash<QString, int> counts;
    for (const QJsonValue &value : std::as_const(m_content))
        ++counts[text(value.toObject(), "status")];
    double max = 1;
    for (int count : std::as_const(counts))
        max = std::max(max, double(count));
    QList<BarRows::Row> rows;
    for (const QString &status : contentStatuses()) {
        const int count = counts.value(status);
        const QColor color = status == QLatin1String("published") ? colors.good
            : status == QLatin1String("review") ? colors.warn : colors.muted;
        rows.append({channelName(status), double(count), max, QString::number(count), color, -1,
                     QStringLiteral("%1 in %2 · click to filter the pipeline").arg(count).arg(status)});
    }
    m_stages->setRows(rows, 92, 32);
    m_stages->setSelected(m_contentStatus->currentIndex() - 1);

    QStringList notes;
    const QString warn = QStringLiteral("<span style=\"color:%1\">⚠</span> ").arg(colors.warn.name());
    QList<QJsonObject> stuck;
    int overdue = 0;
    QList<double> cycles;
    for (const QJsonValue &value : std::as_const(m_content)) {
        const QJsonObject item = value.toObject();
        const QString status = text(item, "status");
        if (status == QLatin1String("review") && number(item, "updatedAt") > 0 &&
            now - number(item, "updatedAt") > 3 * kDay)
            stuck.append(item);
        if ((status == QLatin1String("queued") || status == QLatin1String("scheduled")) &&
            number(item, "scheduledAt") > 0 && number(item, "scheduledAt") < now)
            ++overdue;
        if (status == QLatin1String("published") && number(item, "createdAt") > 0 &&
            number(item, "publishedAt") > number(item, "createdAt"))
            cycles.append(number(item, "publishedAt") - number(item, "createdAt"));
    }
    if (!stuck.isEmpty()) {
        std::sort(stuck.begin(), stuck.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return number(a, "updatedAt") < number(b, "updatedAt");
        });
        const QJsonObject oldest = stuck.first();
        notes << warn + QStringLiteral("<b>%1 in review for 3+ days.</b> Oldest: <a style=\"color:%5\" href=\"content:%2\">“%3”</a>%4")
                            .arg(stuck.size()).arg(escaped(text(oldest, "id")), escaped(text(oldest, "title")),
                                 text(oldest, "owner").isEmpty() ? QString() : QStringLiteral(" (%1)").arg(escaped(text(oldest, "owner"))),
                                 colors.primary.name());
    }
    if (overdue > 0)
        notes << warn + QStringLiteral("<b>%1 past their scheduled time</b> and not marked published.").arg(overdue);
    if (!cycles.isEmpty()) {
        std::sort(cycles.begin(), cycles.end());
        const int middle = int(cycles.size()) / 2;
        const double median = cycles.size() % 2 ? cycles.at(middle) : (cycles.at(middle - 1) + cycles.at(middle)) / 2;
        notes << QStringLiteral("◷ Median created → published: <b>%1 days</b>").arg(median / kDay, 0, 'f', 1);
    }
    if (stuck.isEmpty() && overdue == 0)
        notes.prepend(QStringLiteral("<span style=\"color:%1\">✓</span> Nothing stuck in review or overdue.").arg(colors.good.name()));
    m_flowNotes->setText(notes.join(QStringLiteral("<br>")));
}

void MarketingFeature::renderAdsOverview()
{
    const DashboardColors colors = DashboardColors::from(this);
    clearLayout(m_adsOverviewBox);
    const auto hint = [this](const QString &value) {
        auto *label = new QLabel(value);
        label->setObjectName(QStringLiteral("marketingHint"));
        label->setWordWrap(true);
        m_adsOverviewBox->addWidget(label);
    };
    if (m_ads.isEmpty())
        return hint(QStringLiteral("Plan paid placements on any channel and log what each platform reports. New ad starts one."));
    const QJsonArray currencies = m_adsSummaryData.value(QStringLiteral("currencies")).toArray();
    if (!currencies.isEmpty()) {
        const QJsonObject first = currencies.first().toObject();
        const QString currency = text(first, "currency");
        auto *facts = new QHBoxLayout;
        facts->setSpacing(6);
        const auto fact = [facts](const QString &label, const QString &value) {
            auto *box = new QFrame;
            box->setObjectName(QStringLiteral("marketingFact"));
            auto *layout = new QVBoxLayout(box);
            layout->setContentsMargins(8, 4, 8, 4);
            layout->setSpacing(0);
            auto *name = new QLabel(label);
            name->setObjectName(QStringLiteral("marketingFactLabel"));
            auto *amount = new QLabel(value);
            amount->setObjectName(QStringLiteral("marketingFactValue"));
            amount->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            amount->setToolTip(value);
            layout->addWidget(name);
            layout->addWidget(amount);
            facts->addWidget(box, 1);
        };
        fact(QStringLiteral("Spend · ") + currency, money(first.value(QStringLiteral("spendCents")), currency));
        fact(QStringLiteral("Clicks · CTR"), QStringLiteral("%1 · %2%").arg(compactCount(qint64(number(first, "clicks"))))
                                                 .arg(number(first, "ctr"), 0, 'f', 1));
        fact(QStringLiteral("Conv. · ROAS"), QStringLiteral("%1 · %2").arg(compactCount(qint64(number(first, "conversions"))),
            number(first, "roas") > 0 ? QString::number(number(first, "roas"), 'f', 1) + QLatin1Char('x') : kDash));
        m_adsOverviewBox->addLayout(facts);
    }
    hint(QStringLiteral("%1 live · %2 need attention%3").arg(int(number(m_adsSummaryData, "active")))
             .arg(int(number(m_adsSummaryData, "attention")))
             .arg(currencies.size() > 1 ? QStringLiteral(" · %1 currencies").arg(currencies.size()) : QString()));
    QList<QJsonObject> ads;
    for (const QJsonValue &value : std::as_const(m_ads)) {
        const QString status = text(value.toObject(), "status");
        if (status == QLatin1String("active") || status == QLatin1String("scheduled") || status == QLatin1String("paused"))
            ads.append(value.toObject());
    }
    std::stable_sort(ads.begin(), ads.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const bool left = text(a, "status") == QLatin1String("active");
        const bool right = text(b, "status") == QLatin1String("active");
        return left != right ? left : number(a, "spendCents") > number(b, "spendCents");
    });
    if (ads.isEmpty())
        return hint(QStringLiteral("No live ads."));
    for (int index = 0; index < ads.size() && index < 4; ++index) {
        const QJsonObject ad = ads.at(index);
        const QJsonObject pacing = ad.value(QStringLiteral("pacing")).toObject();
        const QString status = text(ad, "status");
        auto *row = clearWidget();
        auto *layout = new QVBoxLayout(row);
        layout->setContentsMargins(0, 2, 0, 2);
        layout->setSpacing(2);
        auto *head = new QHBoxLayout;
        auto *name = new QLabel(text(ad, "name"));
        name->setObjectName(QStringLiteral("marketingNote"));
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        name->setToolTip(text(ad, "name"));
        head->addWidget(name, 1);
        auto *pill = new QLabel(adLabel(status));
        pill->setObjectName(QStringLiteral("marketingPill"));
        pill->setProperty("tone", status == QLatin1String("active") ? QStringLiteral("good")
                                  : status == QLatin1String("paused") ? QStringLiteral("warn") : QStringLiteral("primary"));
        head->addWidget(pill);
        auto *edit = linkButton(QStringLiteral("Edit"));
        connect(edit, &QPushButton::clicked, this, [this, ad] { editAd(ad); });
        head->addWidget(edit);
        layout->addLayout(head);
        const double total = number(pacing, "totalBudgetCents");
        const QString currency = text(ad, "currency");
        QString detail = channelName(text(ad, "channel"), m_settings, m_channelCatalog) + QStringLiteral(" · ") +
                         money(ad.value(QStringLiteral("spendCents")), currency);
        if (total > 0)
            detail += QStringLiteral(" of ") + money(total, currency);
        if (!text(pacing, "state").isEmpty())
            detail += QStringLiteral(" · ") + adPacingText(text(pacing, "state"));
        auto *small = new QLabel(detail);
        small->setObjectName(QStringLiteral("marketingSmall"));
        small->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(small);
        if (total > 0) {
            auto *bar = new ProgressLine;
            const bool over = text(pacing, "state") == QLatin1String("overspent");
            bar->setProgress(number(ad, "spendCents") / total, over ? colors.bad : colors.series[0],
                             qBound(0.0, number(pacing, "expectedCents") / total, 1.0));
            bar->setToolTip(QStringLiteral("Spent so far; the tick marks where spend should be by today"));
            layout->addWidget(bar);
        }
        m_adsOverviewBox->addWidget(row);
    }
}

void MarketingFeature::renderX()
{
    const QJsonObject x = m_settings.value(QStringLiteral("x")).toObject();
    const bool connected = x.value(QStringLiteral("connected")).toBool();
    const QString username = text(x, "username");
    m_xConnect->setText(connected ? QStringLiteral("Connection ↗") : QStringLiteral("Connect ↗"));
    m_xConnect->setVisible(m_canManageSettings);
    if (connected) {
        const QString link = DashboardColors::from(this).primary.name();
        QString status = QStringLiteral("Posting as <a style=\"color:%3; text-decoration:none\" href=\"https://x.com/%1\"><b>@%2</b></a>")
                             .arg(QString::fromUtf8(QUrl::toPercentEncoding(username)), escaped(username), link);
        if (number(x, "lastPostedAt") > 0) {
            status += QStringLiteral(" · last post %1").arg(escaped(relativeLabel(qint64(number(x, "lastPostedAt")))));
            if (text(x, "lastPostUrl").startsWith(QLatin1String("https://")))
                status += QStringLiteral(" <a style=\"color:%2; text-decoration:none\" href=\"%1\">View ↗</a>")
                              .arg(escaped(text(x, "lastPostUrl")), link);
        }
        m_xStatus->setText(status);
    } else {
        m_xStatus->setText(m_canManageSettings
            ? QStringLiteral("Connect the organization's X account on the web dashboard to post from here.")
            : QStringLiteral("An owner or admin can connect X to post from here."));
    }
    const QString previous = m_xContent->currentData().toString();
    QList<QJsonObject> items;
    for (const QJsonValue &value : std::as_const(m_content)) {
        const QJsonObject item = value.toObject();
        const QString status = text(item, "status");
        if (text(item, "channel") == QLatin1String("x") && status != QLatin1String("published") &&
            status != QLatin1String("archived"))
            items.append(item);
    }
    std::stable_sort(items.begin(), items.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const double left = number(a, "scheduledAt") > 0 ? number(a, "scheduledAt") : 1e18;
        const double right = number(b, "scheduledAt") > 0 ? number(b, "scheduledAt") : 1e18;
        return left < right;
    });
    {
        const QSignalBlocker blocker(m_xContent);
        m_xContent->clear();
        m_xContent->addItem(QStringLiteral("Write a new post"), QString());
        for (const QJsonObject &item : std::as_const(items))
            m_xContent->addItem(text(item, "title") + (number(item, "scheduledAt") > 0
                                    ? QStringLiteral(" · ") + shortDate(qint64(number(item, "scheduledAt"))) : QString()),
                                text(item, "id"));
        m_xContent->setCurrentIndex(qMax(0, m_xContent->findData(previous)));
    }
    m_xContent->setVisible(!items.isEmpty());
    const bool allowed = connected && m_canManageSettings;
    m_xText->setEnabled(allowed);
    m_xPost->setEnabled(allowed);
    updateXCount();
}

void MarketingFeature::updateXCount()
{
    const int length = xLength(m_xText->toPlainText());
    const DashboardColors colors = DashboardColors::from(this);
    m_xCount->setText(length > kXLimit
        ? QStringLiteral("<span style=\"color:%1\">%2 / %3</span>").arg(colors.bad.name()).arg(length).arg(kXLimit)
        : QStringLiteral("%1 / %2").arg(length).arg(kXLimit));
}

void MarketingFeature::composeFromContent(const QJsonObject &content)
{
    if (content.isEmpty())
        return;
    const int index = text(content, "channel") == QLatin1String("x") ? m_xContent->findData(text(content, "id")) : -1;
    m_xContent->setCurrentIndex(qMax(0, index));
    m_xText->setPlainText(text(content, "body").isEmpty() ? text(content, "title")
                                                          : content.value(QStringLiteral("body")).toString());
    m_xContentId = text(content, "id");
    reveal(m_xPanel);
    m_xText->setFocus(Qt::OtherFocusReason);
}

void MarketingFeature::postToX()
{
    const QString post = m_xText->toPlainText().trimmed();
    if (post.isEmpty())
        return;
    const int length = xLength(post);
    if (length > kXLimit && QMessageBox::question(this, QStringLiteral("Post to X"),
            QStringLiteral("This post is %1 characters. Only X Premium accounts can post more than %2. Post anyway?")
                .arg(length).arg(kXLimit)) != QMessageBox::Yes)
        return;
    const QString username = text(m_settings.value(QStringLiteral("x")).toObject(), "username");
    if (QMessageBox::question(this, QStringLiteral("Post to X"),
                              QStringLiteral("Post this to X as @%1?\n\n%2").arg(username, post)) != QMessageBox::Yes)
        return;
    const QString contentId = m_xContent->currentData().toString().isEmpty() ? m_xContentId
                                                                             : m_xContent->currentData().toString();
    m_xPost->setEnabled(false);
    m_xPost->setText(QStringLiteral("Posting…"));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/x-post")),
            QJsonObject{{QStringLiteral("text"), post}, {QStringLiteral("contentId"), contentId}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_xPost->setText(QStringLiteral("Post to X"));
        if (!ok) {
            setStatus(QStringLiteral("X did not accept the post: %1%2").arg(error,
                          text(payload, "detail").isEmpty() ? QString() : QStringLiteral(" (%1)").arg(text(payload, "detail"))), true);
            return renderX();
        }
        m_xText->clear();
        m_xContentId.clear();
        setStatus(QStringLiteral("Posted to X: %1").arg(text(payload, "url")));
        loadOrganization();
    });
}

void MarketingFeature::renderWeek()
{
    const DashboardColors colors = DashboardColors::from(this);
    clearLayout(m_weekBox);
    const QDate today = QDate::currentDate();
    m_weekPanel->hint()->setText(today.toString(QStringLiteral("MMM d")) + QStringLiteral(" – ") +
                                 today.addDays(6).toString(QStringLiteral("MMM d")));
    QHash<QString, QColor> slotColors;
    for (const auto &entry : channelColors(seriesByChannel(m_metrics), colors))
        slotColors.insert(entry.first, entry.second);
    for (int offset = 0; offset < 7; ++offset) {
        const QDate date = today.addDays(offset);
        const qint64 from = QDateTime(date, QTime(0, 0)).toMSecsSinceEpoch();
        const qint64 to = QDateTime(date.addDays(1), QTime(0, 0)).toMSecsSinceEpoch();
        QList<QJsonObject> items;
        for (const QJsonValue &value : std::as_const(m_content)) {
            const QJsonObject item = value.toObject();
            const qint64 at = qint64(number(item, "scheduledAt"));
            if (at >= from && at < to && text(item, "status") != QLatin1String("archived"))
                items.append(item);
        }
        std::sort(items.begin(), items.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return number(a, "scheduledAt") < number(b, "scheduledAt");
        });
        auto *day = new QFrame;
        day->setObjectName(QStringLiteral("marketingDay"));
        day->setProperty("last", offset == 6);
        auto *layout = new QHBoxLayout(day);
        layout->setContentsMargins(0, 3, 0, 3);
        layout->setSpacing(10);
        auto *dateBox = new QVBoxLayout;
        dateBox->setSpacing(0);
        auto *dayName = new QLabel(offset == 0 ? QStringLiteral("TODAY") : date.toString(QStringLiteral("ddd")).toUpper());
        dayName->setObjectName(QStringLiteral("marketingDayName"));
        dayName->setAlignment(Qt::AlignHCenter);
        auto *dayNumber = new QLabel(QString::number(date.day()));
        dayNumber->setObjectName(QStringLiteral("marketingDayNumber"));
        dayNumber->setProperty("today", offset == 0);
        dayNumber->setAlignment(Qt::AlignHCenter);
        dateBox->addWidget(dayName);
        dateBox->addWidget(dayNumber);
        auto *dateWidget = clearWidget();
        dateWidget->setLayout(dateBox);
        dateWidget->setFixedWidth(44);
        dateBox->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(dateWidget, 0, Qt::AlignTop);
        auto *daySlots = new QVBoxLayout;
        daySlots->setSpacing(2);
        for (const QJsonObject &item : std::as_const(items)) {
            const QString label = QDateTime::fromMSecsSinceEpoch(qint64(number(item, "scheduledAt"))).toString(QStringLiteral("HH:mm")) +
                                  QStringLiteral("   ") + text(item, "title");
            auto *slot = new QPushButton(fontMetrics().elidedText(label, Qt::ElideRight, 260));
            slot->setObjectName(QStringLiteral("marketingSlot"));
            slot->setCursor(Qt::PointingHandCursor);
            slot->setToolTip(QStringLiteral("%1 · %2 · %3").arg(text(item, "title"),
                channelName(text(item, "channel"), m_settings, m_channelCatalog), text(item, "status")));
            slot->setStyleSheet(QStringLiteral("QPushButton#marketingSlot { border-left: 3px solid %1; border-radius: 0; }")
                                    .arg(slotColors.value(text(item, "channel"), brandInk(text(item, "channel"), colors)).name()));
            slot->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            connect(slot, &QPushButton::clicked, this, [this, item] { editContent(item); });
            daySlots->addWidget(slot);
        }
        if (items.isEmpty()) {
            auto *empty = new QLabel(QStringLiteral("Nothing scheduled"));
            empty->setObjectName(QStringLiteral("marketingHint"));
            daySlots->addWidget(empty);
        }
        daySlots->addStretch();
        layout->addLayout(daySlots, 1);
        m_weekBox->addWidget(day);
    }
}

void MarketingFeature::renderChart()
{
    const DashboardColors colors = DashboardColors::from(this);
    static const char *keys[] = {"reach", "engagements", "followers"};
    static const char *labels[] = {"Reach", "Engagements", "Followers"};
    const int metric = qBound(0, m_chartMetric->checkedId(), 2);
    m_chartPanel->titleLabel()->setText(QStringLiteral("%1 by channel").arg(QLatin1String(labels[metric])));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 start = now - m_rangeDays * kDay;
    const MetricSeries series = seriesByChannel(m_metrics);
    QList<LineChart::Series> lines;
    QStringList legend;
    for (const auto &[channel, color] : channelColors(series, colors)) {
        const QList<QJsonObject> list = series.value(channel);
        LineChart::Series line;
        line.name = channelName(channel, m_settings, m_channelCatalog);
        line.color = color;
        const QJsonObject carried = snapshotAt(list, start);
        if (!carried.isEmpty())
            line.points.append({start, number(carried, keys[metric]), false});
        for (const QJsonObject &item : list) {
            const qint64 at = qint64(number(item, "observedAt"));
            line.history.append({at, number(item, keys[metric]), true});
            if (at > start)
                line.points.append({at, number(item, keys[metric]), true});
        }
        if (line.points.isEmpty())
            continue;
        legend << QStringLiteral("<span style=\"color:%1\">■</span> %2").arg(color.name(), escaped(line.name));
        lines.append(line);
    }
    if (!lines.isEmpty())
        legend << QStringLiteral("● logged snapshot · ○ carried from before");
    m_chartLegend->setText(legend.join(QStringLiteral(" &nbsp;&nbsp; ")));
    m_chartLegend->setVisible(!lines.isEmpty());
    m_chart->setSeries(lines, start, now,
                       QStringLiteral("No metric snapshots in the last %1 days.\nRefresh a channel or log its numbers with Add metrics.")
                           .arg(m_rangeDays));
}

void MarketingFeature::renderLanding()
{
    const DashboardColors colors = DashboardColors::from(this);
    const bool ready = text(m_traffic, "status") == QLatin1String("ok");
    QList<BarRows::Row> rows;
    if (ready) {
        const QJsonArray landing = m_traffic.value(QStringLiteral("landing")).toArray();
        double max = 1;
        for (int index = 0; index < landing.size() && index < 8; ++index)
            max = std::max(max, number(landing.at(index).toObject(), "sessions"));
        for (int index = 0; index < landing.size() && index < 8; ++index) {
            const QJsonObject row = landing.at(index).toObject();
            rows.append({text(row, "landing"), number(row, "sessions"), max,
                         compactCount(qint64(number(row, "sessions"))), colors.series[0], -1,
                         QStringLiteral("%1: %2 visits, %3 bounce").arg(text(row, "landing"),
                             fullCount(number(row, "sessions")), shareText(row.value(QStringLiteral("bounceRate"))))});
        }
    }
    m_landing->setEmptyText(ready ? QStringLiteral("No landing pages in this window.")
                            : m_trafficLoaded ? QStringLiteral("Landing pages appear once Analytics records visits.")
                                              : QStringLiteral("Loading…"));
    m_landing->setRows(rows, -45, 48);
}

void MarketingFeature::renderReferrers()
{
    if (!m_referrers)
        return;
    const QString status = text(m_referringSites, "status");
    m_referrersPanel->setVisible(status != QLatin1String("other_site"));
    const QString host = text(m_referringSites, "host");
    const bool ready = status == QLatin1String("ok");
    const DashboardColors colors = DashboardColors::from(this);
    const QJsonArray board = m_referringSites.value(QStringLiteral("board")).toArray();
    QList<BarRows::Row> rows;
    if (ready) {
        double max = 1;
        for (const QJsonValue &value : board)
            max = std::max(max, number(value.toObject(), "visits"));
        for (const QJsonValue &value : board) {
            const QJsonObject row = value.toObject();
            const qint64 lastAt = qint64(number(row, "lastAt"));
            rows.append({text(row, "host"), number(row, "visits"), max,
                         compactCount(qint64(number(row, "visits"))), colors.series[0], -1,
                         QStringLiteral("%1: %2 visits, last %3%4").arg(text(row, "host"),
                             fullCount(number(row, "visits")),
                             lastAt > 0 ? relativeLabel(lastAt) : QStringLiteral("unknown"),
                             text(row, "url").isEmpty() ? QString() : QStringLiteral("\nClick to open %1").arg(text(row, "url")))});
        }
    }
    m_referrersPanel->hint()->setText(ready ? QStringLiteral("%1 visits · %2 sites · all time").arg(
        fullCount(number(m_referringSites, "visits")), fullCount(number(m_referringSites, "sites"))) : QString());
    m_referrers->setEmptyText(ready ? QStringLiteral("No other website has sent a visitor to %1 yet.").arg(host)
                              : m_referringSites.isEmpty() ? QStringLiteral("Loading…")
                                                           : QStringLiteral("Referring websites could not be read just now."));
    m_referrers->setRows(rows, -45, 48);
    m_referrersNote->setText(ready && !board.isEmpty()
        ? QStringLiteral("Page loads that another website linked to %1, from the browser's Referer header.").arg(host)
        : QString());
    m_referrersNote->setVisible(!m_referrersNote->text().isEmpty());
}

void MarketingFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setVisible(!value.isEmpty());
    m_status->setProperty("error", error);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

} // namespace forkmesh::marketing

namespace {

const forkmesh::market::NativePackageRegistration marketingRegistration({
    QStringLiteral("builtin:marketing"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Marketing"),
    // The desktop icon set ships Octicons. "megaphone" is a Lucide web icon
    // name and therefore fell through to a blank desktop rail button.
    QStringLiteral("broadcast"),
    QStringLiteral("/api/marketing"),
    QStringLiteral("forkmesh-marketing-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::marketing::MarketingFeature(host, parent);
    },
});

} // namespace
