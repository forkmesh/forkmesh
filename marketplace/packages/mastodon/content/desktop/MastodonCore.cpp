#include "MastodonCore.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace forkmesh::mastodon {

namespace {

QString decodeEntities(const QString &text)
{
    if (!text.contains(QLatin1Char('&')))
        return text;
    static const QRegularExpression entity(QStringLiteral("&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);"));
    static const QHash<QString, QString> named{
        {QStringLiteral("amp"), QStringLiteral("&")},   {QStringLiteral("lt"), QStringLiteral("<")},
        {QStringLiteral("gt"), QStringLiteral(">")},    {QStringLiteral("quot"), QStringLiteral("\"")},
        {QStringLiteral("apos"), QStringLiteral("'")},  {QStringLiteral("nbsp"), QString(QChar(0x00A0))},
        {QStringLiteral("hellip"), QString(QChar(0x2026))}, {QStringLiteral("mdash"), QString(QChar(0x2014))},
        {QStringLiteral("ndash"), QString(QChar(0x2013))},
    };
    QString out;
    out.reserve(text.size());
    qsizetype last = 0;
    auto it = entity.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += text.mid(last, match.capturedStart() - last);
        const QString body = match.captured(1);
        QString replacement = match.captured(0);
        if (body.startsWith(QLatin1String("#x")) || body.startsWith(QLatin1String("#X"))) {
            bool ok = false;
            const uint code = body.mid(2).toUInt(&ok, 16);
            if (ok && code > 0 && code <= 0x10FFFF)
                replacement = QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1);
        } else if (body.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            const uint code = body.mid(1).toUInt(&ok, 10);
            if (ok && code > 0 && code <= 0x10FFFF)
                replacement = QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1);
        } else if (named.contains(body)) {
            replacement = named.value(body);
        }
        out += replacement;
        last = match.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

QString attribute(const QString &attributes, const QString &name)
{
    const QRegularExpression pattern(
        QStringLiteral("(?:^|\\s)%1\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))")
            .arg(QRegularExpression::escape(name)),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(attributes);
    if (!match.hasMatch())
        return {};
    for (int group = 1; group <= 3; ++group) {
        if (match.capturedStart(group) >= 0)
            return decodeEntities(match.captured(group));
    }
    return {};
}

QString tagFromHref(const QString &href)
{
    const QString path = QUrl(href).path();
    const int index = path.indexOf(QLatin1String("/tags/"));
    if (index < 0)
        return {};
    return QUrl::fromPercentEncoding(path.mid(index + 6).section(QLatin1Char('/'), 0, 0).toUtf8());
}

QString base64Url(const QByteArray &bytes)
{
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

}

QString SavedAccount::key() const
{
    return instance + QLatin1Char('|') + (id.isEmpty() ? acct : id);
}

QString SavedAccount::handle() const
{
    if (acct.isEmpty())
        return instanceHost(instance);
    return QLatin1Char('@') + acct + QLatin1Char('@') + instanceHost(instance);
}

QJsonObject SavedAccount::toJson() const
{
    return {
        {QStringLiteral("instance"), instance}, {QStringLiteral("token"), token},
        {QStringLiteral("clientId"), clientId}, {QStringLiteral("clientSecret"), clientSecret},
        {QStringLiteral("id"), id},             {QStringLiteral("acct"), acct},
        {QStringLiteral("displayName"), displayName}, {QStringLiteral("avatar"), avatar},
    };
}

SavedAccount SavedAccount::fromJson(const QJsonObject &object)
{
    SavedAccount account;
    account.instance = normalizeInstance(object.value(QStringLiteral("instance")).toString());
    account.token = object.value(QStringLiteral("token")).toString();
    account.clientId = object.value(QStringLiteral("clientId")).toString();
    account.clientSecret = object.value(QStringLiteral("clientSecret")).toString();
    account.id = object.value(QStringLiteral("id")).toString();
    account.acct = object.value(QStringLiteral("acct")).toString();
    account.displayName = object.value(QStringLiteral("displayName")).toString();
    account.avatar = object.value(QStringLiteral("avatar")).toString();
    return account;
}

bool SavedAccount::valid() const
{
    return !instance.isEmpty() && !token.isEmpty();
}

QList<SavedAccount> accountsFromJson(const QByteArray &json)
{
    QList<SavedAccount> accounts;
    const QJsonArray rows = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &row : rows) {
        const SavedAccount account = SavedAccount::fromJson(row.toObject());
        if (account.valid())
            accounts.append(account);
    }
    return accounts;
}

QByteArray accountsToJson(const QList<SavedAccount> &accounts)
{
    QJsonArray rows;
    for (const SavedAccount &account : accounts)
        rows.append(account.toJson());
    return QJsonDocument(rows).toJson(QJsonDocument::Compact);
}

QString normalizeInstance(const QString &raw)
{
    QString text = raw.trimmed();
    if (text.isEmpty())
        return {};
    if (text.startsWith(QLatin1Char('@')) && text.count(QLatin1Char('@')) == 2)
        text = text.section(QLatin1Char('@'), 2);
    else if (!text.contains(QLatin1String("://")) && text.count(QLatin1Char('@')) == 1)
        text = text.section(QLatin1Char('@'), 1);
    if (!text.contains(QLatin1String("://")))
        text.prepend(QStringLiteral("https://"));
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty())
        return {};
    if (!url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return {};
    if (!url.path().isEmpty() && url.path() != QLatin1String("/"))
        return {};
    if (!url.host().contains(QLatin1Char('.')) && url.host() != QLatin1String("localhost"))
        return {};
    QString origin = QStringLiteral("https://") + url.host().toLower();
    if (url.port() > 0 && url.port() != 443)
        origin += QLatin1Char(':') + QString::number(url.port());
    return origin;
}

QString instanceHost(const QString &instance)
{
    return QUrl(instance).host();
}

bool safeWebUrl(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty() &&
           (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"));
}

QString escapeText(const QString &text)
{
    return text.toHtmlEscaped();
}

QString elide(const QString &text, int length)
{
    const QString flat = text.simplified();
    if (flat.size() <= length)
        return flat;
    return flat.left(qMax(0, length - 1)).trimmed() + QChar(0x2026);
}

RichText renderContent(const QString &html, const QJsonArray &mentions, const QJsonArray &tags)
{
    QHash<QString, QString> mentionIds;
    for (const QJsonValue &value : mentions) {
        const QJsonObject mention = value.toObject();
        mentionIds.insert(mention.value(QStringLiteral("url")).toString(),
                          mention.value(QStringLiteral("id")).toString());
    }
    QHash<QString, QString> tagNames;
    for (const QJsonValue &value : tags) {
        const QJsonObject tag = value.toObject();
        tagNames.insert(tag.value(QStringLiteral("url")).toString(),
                        tag.value(QStringLiteral("name")).toString());
    }

    static const QRegularExpression tagPattern(QStringLiteral("<\\s*(/?)\\s*([a-zA-Z0-9]+)([^>]*)>"));
    RichText result;
    QList<QString> spans;
    QList<bool> openLinks;
    int hidden = 0;
    bool pendingBreak = false;

    const auto breakLine = [&](bool paragraph) {
        if (result.html.isEmpty())
            return;
        pendingBreak = false;
        result.html += paragraph ? QStringLiteral("<br><br>") : QStringLiteral("<br>");
        result.plain += paragraph ? QStringLiteral("\n\n") : QStringLiteral("\n");
    };
    const auto appendText = [&](const QString &raw) {
        if (hidden > 0 || raw.isEmpty())
            return;
        const QString text = decodeEntities(raw);
        if (pendingBreak && !text.trimmed().isEmpty())
            breakLine(true);
        result.html += text.toHtmlEscaped();
        result.plain += text;
    };

    qsizetype last = 0;
    auto it = tagPattern.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        appendText(html.mid(last, match.capturedStart() - last));
        last = match.capturedEnd();
        const bool closing = !match.captured(1).isEmpty();
        const QString name = match.captured(2).toLower();
        const QString attributes = match.captured(3);
        if (name == QLatin1String("br")) {
            if (hidden == 0) {
                result.html += QStringLiteral("<br>");
                result.plain += QLatin1Char('\n');
            }
        } else if (name == QLatin1String("p") || name == QLatin1String("blockquote") ||
                   name == QLatin1String("ul") || name == QLatin1String("ol") ||
                   name == QLatin1String("pre") || name == QLatin1String("div") ||
                   (name.size() == 2 && name.at(0) == QLatin1Char('h') && name.at(1).isDigit())) {
            if (!result.html.isEmpty())
                pendingBreak = true;
        } else if (name == QLatin1String("script") || name == QLatin1String("style")) {
            hidden = closing ? qMax(0, hidden - 1) : hidden + 1;
        } else if (name == QLatin1String("li")) {
            if (!closing && hidden == 0) {
                if (!result.html.isEmpty())
                    breakLine(false);
                pendingBreak = false;
                result.html += QStringLiteral("&#8226; ");
                result.plain += QStringLiteral("\u2022 ");
            }
        } else if (name == QLatin1String("a")) {
            if (closing) {
                if (!openLinks.isEmpty() && openLinks.takeLast())
                    result.html += QStringLiteral("</a>");
                continue;
            }
            if (pendingBreak)
                breakLine(true);
            const QString href = attribute(attributes, QStringLiteral("href"));
            const QString classes = attribute(attributes, QStringLiteral("class"));
            QString target;
            if (mentionIds.contains(href)) {
                target = kAccountScheme + QLatin1Char(':') + mentionIds.value(href);
            } else if (tagNames.contains(href)) {
                target = kTagScheme + QLatin1Char(':') + tagNames.value(href);
            } else if (classes.contains(QLatin1String("hashtag")) && !tagFromHref(href).isEmpty()) {
                target = kTagScheme + QLatin1Char(':') + tagFromHref(href);
            } else if (safeWebUrl(QUrl(href))) {
                target = QUrl(href).toString(QUrl::FullyEncoded);
            }
            openLinks.append(!target.isEmpty());
            if (!target.isEmpty())
                result.html += QStringLiteral("<a href=\"%1\">").arg(target.toHtmlEscaped());
        } else if (name == QLatin1String("span")) {
            if (closing) {
                if (spans.isEmpty())
                    continue;
                const QString classes = spans.takeLast();
                if (classes.contains(QLatin1String("invisible")))
                    hidden = qMax(0, hidden - 1);
                else if (classes.contains(QLatin1String("ellipsis")) && hidden == 0) {
                    result.html += QChar(0x2026);
                    result.plain += QChar(0x2026);
                }
                continue;
            }
            const QString classes = attribute(attributes, QStringLiteral("class"));
            spans.append(classes);
            if (classes.split(QLatin1Char(' ')).contains(QLatin1String("invisible")))
                ++hidden;
        } else if (name == QLatin1String("b") || name == QLatin1String("strong") ||
                   name == QLatin1String("i") || name == QLatin1String("em") ||
                   name == QLatin1String("code") || name == QLatin1String("del") ||
                   name == QLatin1String("s")) {
            if (hidden > 0)
                continue;
            if (!closing && pendingBreak)
                breakLine(true);
            static const QHash<QString, QString> simple{
                {QStringLiteral("strong"), QStringLiteral("b")}, {QStringLiteral("em"), QStringLiteral("i")},
                {QStringLiteral("del"), QStringLiteral("s")},
            };
            const QString shown = simple.value(name, name);
            result.html += closing ? QStringLiteral("</%1>").arg(shown) : QStringLiteral("<%1>").arg(shown);
        }
    }
    appendText(html.mid(last));
    while (!openLinks.isEmpty()) {
        if (openLinks.takeLast())
            result.html += QStringLiteral("</a>");
    }
    result.plain = result.plain.trimmed();
    return result;
}

QString plainText(const QString &html)
{
    return renderContent(html).plain;
}

QString relativeTime(const QDateTime &when, const QDateTime &now)
{
    if (!when.isValid())
        return {};
    const qint64 seconds = when.secsTo(now);
    if (seconds < 60)
        return QStringLiteral("now");
    if (seconds < 3600)
        return QStringLiteral("%1m").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1h").arg(seconds / 3600);
    if (seconds < 7 * 86400)
        return QStringLiteral("%1d").arg(seconds / 86400);
    const QDate date = when.toLocalTime().date();
    if (date.year() == now.toLocalTime().date().year())
        return QLocale().toString(date, QStringLiteral("d MMM"));
    return QLocale().toString(date, QStringLiteral("d MMM yyyy"));
}

QString relativeTime(const QString &iso, const QDateTime &now)
{
    return relativeTime(QDateTime::fromString(iso, Qt::ISODateWithMs), now);
}

QString countText(qint64 count)
{
    if (count < 1000)
        return QString::number(qMax<qint64>(0, count));
    if (count < 1000000) {
        const double value = count / 1000.0;
        return QString::number(value, 'f', value < 10 ? 1 : 0).remove(QStringLiteral(".0")) + QLatin1Char('K');
    }
    const double value = count / 1000000.0;
    return QString::number(value, 'f', value < 10 ? 1 : 0).remove(QStringLiteral(".0")) + QLatin1Char('M');
}

QUrl linkTarget(const QByteArray &linkHeader, const QString &relation)
{
    static const QRegularExpression link(QStringLiteral("<([^>]+)>\\s*;\\s*rel=\"?([a-zA-Z ]+)\"?"));
    auto it = link.globalMatch(QString::fromUtf8(linkHeader));
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (match.captured(2).split(QLatin1Char(' ')).contains(relation))
            return QUrl(match.captured(1));
    }
    return {};
}

QString codeFromCallback(const QString &pasted)
{
    const QString text = pasted.trimmed();
    if (text.contains(QLatin1String("code="))) {
        QString query = text.section(QLatin1Char('?'), 1);
        if (query.isEmpty())
            query = text;
        return QUrlQuery(query.section(QLatin1Char('#'), 0, 0)).queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    }
    static const QRegularExpression code(QStringLiteral("^[A-Za-z0-9_\\-]{8,}$"));
    return code.match(text).hasMatch() ? text : QString();
}

QUrlQuery callbackQuery(const QByteArray &requestHead)
{
    const QByteArray line = requestHead.left(requestHead.indexOf('\n')).trimmed();
    const QList<QByteArray> parts = line.split(' ');
    if (parts.size() < 2 || parts.at(0) != "GET")
        return {};
    const QUrl url(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(parts.at(1)));
    if (url.path() != QLatin1String("/callback"))
        return {};
    return QUrlQuery(url);
}

QString randomToken(int bytes)
{
    QByteArray data(bytes, Qt::Uninitialized);
    for (int index = 0; index < bytes; ++index)
        data[index] = static_cast<char>(QRandomGenerator::system()->bounded(256));
    return base64Url(data);
}

QString pkceChallenge(const QString &verifier)
{
    return base64Url(QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256));
}

QUrl authorizeUrl(const QString &instance, const QString &clientId, const QString &redirect,
                  const QString &state, const QString &challenge)
{
    QUrl url(instance + QStringLiteral("/oauth/authorize"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), clientId);
    query.addQueryItem(QStringLiteral("redirect_uri"), redirect);
    query.addQueryItem(QStringLiteral("scope"), kScopes);
    query.addQueryItem(QStringLiteral("state"), state);
    if (!challenge.isEmpty()) {
        query.addQueryItem(QStringLiteral("code_challenge"), challenge);
        query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    }
    url.setQuery(query);
    return url;
}

QString displayName(const QJsonObject &account)
{
    const QString name = account.value(QStringLiteral("display_name")).toString().trimmed();
    if (!name.isEmpty())
        return name;
    return account.value(QStringLiteral("username")).toString();
}

QString fullHandle(const QJsonObject &account, const QString &instance)
{
    const QString acct = account.value(QStringLiteral("acct")).toString();
    if (acct.isEmpty())
        return {};
    if (acct.contains(QLatin1Char('@')) || instance.isEmpty())
        return QLatin1Char('@') + acct;
    return QLatin1Char('@') + acct + QLatin1Char('@') + instanceHost(instance);
}

QJsonObject shownStatus(const QJsonObject &status)
{
    const QJsonObject reblog = status.value(QStringLiteral("reblog")).toObject();
    return reblog.isEmpty() ? status : reblog;
}

QStringList visibilityValues()
{
    return {QStringLiteral("public"), QStringLiteral("unlisted"), QStringLiteral("private"),
            QStringLiteral("direct")};
}

QString visibilityLabel(const QString &visibility)
{
    if (visibility == QLatin1String("unlisted"))
        return QStringLiteral("Quiet public");
    if (visibility == QLatin1String("private"))
        return QStringLiteral("Followers only");
    if (visibility == QLatin1String("direct"))
        return QStringLiteral("Private mention");
    return QStringLiteral("Public");
}

QString visibilityIcon(const QString &visibility)
{
    if (visibility == QLatin1String("unlisted"))
        return QStringLiteral("moon");
    if (visibility == QLatin1String("private"))
        return QStringLiteral("lock");
    if (visibility == QLatin1String("direct"))
        return QStringLiteral("at");
    return QStringLiteral("globe");
}

QString notificationSentence(const QString &type)
{
    static const QHash<QString, QString> sentences{
        {QStringLiteral("favourite"), QStringLiteral("favourited your post")},
        {QStringLiteral("reblog"), QStringLiteral("boosted your post")},
        {QStringLiteral("follow"), QStringLiteral("followed you")},
        {QStringLiteral("follow_request"), QStringLiteral("asked to follow you")},
        {QStringLiteral("mention"), QStringLiteral("mentioned you")},
        {QStringLiteral("status"), QStringLiteral("just posted")},
        {QStringLiteral("poll"), QStringLiteral("a poll you took part in has ended")},
        {QStringLiteral("update"), QStringLiteral("edited a post you interacted with")},
        {QStringLiteral("quote"), QStringLiteral("quoted your post")},
        {QStringLiteral("admin.sign_up"), QStringLiteral("signed up")},
        {QStringLiteral("admin.report"), QStringLiteral("filed a report")},
        {QStringLiteral("severed_relationships"), QStringLiteral("some of your follow relationships were severed")},
        {QStringLiteral("moderation_warning"), QStringLiteral("you received a moderation warning")},
        {QStringLiteral("annual_report"), QStringLiteral("your year in review is ready")},
    };
    return sentences.value(type, QStringLiteral("sent a notification"));
}

QString filterHideReason(const QJsonObject &status, const QString &context, bool *hide)
{
    if (hide)
        *hide = false;
    QStringList titles;
    for (const QJsonValue &value : status.value(QStringLiteral("filtered")).toArray()) {
        const QJsonObject filter = value.toObject().value(QStringLiteral("filter")).toObject();
        const QJsonArray contexts = filter.value(QStringLiteral("context")).toArray();
        if (!context.isEmpty() && !contexts.isEmpty() && !contexts.contains(context))
            continue;
        if (filter.value(QStringLiteral("filter_action")).toString() == QLatin1String("hide")) {
            if (hide)
                *hide = true;
        }
        titles.append(filter.value(QStringLiteral("title")).toString());
    }
    return titles.join(QStringLiteral(", "));
}

int statusLength(const QString &text)
{
    static const QRegularExpression url(QStringLiteral("https?://[^\\s]+"));
    static const QRegularExpression mention(QStringLiteral("(@[A-Za-z0-9_]+)@[A-Za-z0-9.\\-]+[A-Za-z0-9]"));
    QString counted = text;
    counted.replace(url, QString(23, QLatin1Char('x')));
    counted.replace(mention, QStringLiteral("\\1"));
    return static_cast<int>(counted.toUcs4().size());
}

}
