#include "DocsLibrary.h"

#include "MarkdownRender.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace forkmesh::docs {
namespace {

const QString kWebOrigin = QStringLiteral("https://forkmesh.com");
const QString kSourceTree = QStringLiteral("https://app.forkmesh.com/forkmesh/forkmesh/tree/main/");
const QString kRepositoryPrefix = QStringLiteral("www/docs/");
constexpr int kDescriptionLimit = 180;

const QStringList kGuideOrder = {
    QStringLiteral("README.md"),
    QStringLiteral("getting-started.md"),
    QStringLiteral("product-promise.md"),
    QStringLiteral("category-comparisons.md"),
    QStringLiteral("terminology.md"),
    QStringLiteral("business-model.md"),
    QStringLiteral("incentive-independence.md"),
    QStringLiteral("git-compatibility.md"),
    QStringLiteral("qt-client.md"),
    QStringLiteral("capability-matrix.md"),
    QStringLiteral("install-support-matrix.md"),
    QStringLiteral("portable-identities.md"),
    QStringLiteral("social-signals.md"),
    QStringLiteral("hosted-repositories.md"),
    QStringLiteral("repository-imports.md"),
    QStringLiteral("admin-dashboard.md"),
    QStringLiteral("cloudflare-workers-ai-coding-agent.md"),
};

const QStringList kReservedSlugs = {
    QStringLiteral("business-model"),
    QStringLiteral("certification"),
    QStringLiteral("cloudflare-worker"),
    QStringLiteral("contributions"),
    QStringLiteral("dependency-audit"),
    QStringLiteral("documents"),
    QStringLiteral("funding"),
    QStringLiteral("git-compatibility"),
    QStringLiteral("governance"),
    QStringLiteral("history"),
    QStringLiteral("onboarding"),
    QStringLiteral("product-promise"),
    QStringLiteral("protocol"),
    QStringLiteral("qt-client"),
};

const QHash<QString, QString> &directoryAreas()
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("design"), QStringLiteral("design")},
        {QStringLiteral("engineering"), QStringLiteral("engineering")},
        {QStringLiteral("security"), QStringLiteral("security")},
        {QStringLiteral("operations"), QStringLiteral("operations")},
        {QStringLiteral("mobile"), QStringLiteral("mobile")},
        {QStringLiteral("research"), QStringLiteral("strategy")},
        {QStringLiteral("strategy"), QStringLiteral("strategy")},
        {QStringLiteral("launch"), QStringLiteral("strategy")},
        {QStringLiteral("superpowers"), QStringLiteral("superpowers")},
    };
    return map;
}

QString stripSpans(QString text)
{
    static const QRegularExpression autolink(QStringLiteral("<((?:https?://|mailto:)[^\\s<>]+)>"));
    static const QRegularExpression tag(QStringLiteral("</?[A-Za-z][A-Za-z0-9]*(?:\\s[^<>]*)?/?>"));
    static const QRegularExpression emphasis(QStringLiteral("(\\*\\*|__|\\*)"));
    text.replace(autolink, QStringLiteral("\\1"));
    text.remove(tag);
    text.remove(emphasis);
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    return text;
}

QString stripInline(QString text)
{
    static const QRegularExpression image(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression link(QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression reference(QStringLiteral("\\[([^\\]]*)\\]\\[[^\\]]*\\]"));
    static const QRegularExpression code(QStringLiteral("(`+)(.+?)\\1"));
    text.replace(image, QStringLiteral("\\1"));
    text.replace(link, QStringLiteral("\\1"));
    text.replace(reference, QStringLiteral("\\1"));
    QString out;
    qsizetype from = 0;
    QRegularExpressionMatchIterator it = code.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += stripSpans(text.mid(from, match.capturedStart() - from));
        out += match.captured(2).trimmed();
        from = match.capturedEnd();
    }
    out += stripSpans(text.mid(from));
    out.remove(QLatin1Char('`'));
    return out.simplified();
}

QString truncated(QString text, int limit)
{
    text = text.simplified();
    while (text.endsWith(QLatin1Char(':')))
        text.chop(1);
    if (text.size() <= limit)
        return text;
    QString cut = text.left(limit - 1);
    const int space = cut.lastIndexOf(QLatin1Char(' '));
    if (space > 0)
        cut.truncate(space);
    while (!cut.isEmpty() && QStringLiteral(",;:").contains(cut.back()))
        cut.chop(1);
    return cut + QChar(0x2026);
}

bool isFence(const QString &trimmed)
{
    return trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"));
}

QStringList proseLines(const QString &markdown)
{
    QStringList out;
    bool fenced = false;
    for (const QString &line : markdown.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (isFence(trimmed)) {
            fenced = !fenced;
            out.append(QString());
            continue;
        }
        out.append(fenced ? QString() : line);
    }
    return out;
}

QString assetTitle(const QString &path)
{
    const QString name = path.section(QLatin1Char('/'), -1);
    const QString suffix = QStringLiteral(".schema.json");
    if (!name.endsWith(suffix))
        return name;
    static const QHash<QString, QString> acronyms = {
        {QStringLiteral("ssh"), QStringLiteral("SSH")},
        {QStringLiteral("https"), QStringLiteral("HTTPS")},
        {QStringLiteral("api"), QStringLiteral("API")},
        {QStringLiteral("apns"), QStringLiteral("APNs")},
    };
    QStringList words = name.chopped(suffix.size()).split(QLatin1Char('-'));
    for (QString &word : words)
        word = acronyms.value(word, word);
    if (!words.isEmpty() && !acronyms.values().contains(words.first()) && !words.first().isEmpty())
        words.first()[0] = words.first().at(0).toUpper();
    return words.join(QLatin1Char(' ')) + QStringLiteral(" schema");
}

QString assetDescription(const QByteArray &bytes)
{
    const QJsonObject object = QJsonDocument::fromJson(bytes).object();
    for (const QString &key :
         {QStringLiteral("description"), QStringLiteral("title"), QStringLiteral("$comment")}) {
        const QString value = object.value(key).toString().trimmed();
        if (!value.isEmpty())
            return truncated(value, kDescriptionLimit);
    }
    return QStringLiteral("Machine-readable data file.");
}

QString snippetFor(const QString &markdown, const QString &needle)
{
    const int at = markdown.indexOf(needle, 0, Qt::CaseInsensitive);
    if (at < 0)
        return {};
    const int start = markdown.lastIndexOf(QLatin1Char('\n'), at) + 1;
    int end = markdown.indexOf(QLatin1Char('\n'), at);
    if (end < 0)
        end = markdown.size();
    QString line = markdown.mid(start, end - start).trimmed();
    static const QRegularExpression lead(QStringLiteral("^([#>|*+-]+|\\d+\\.)\\s*"));
    line.remove(lead);
    line = stripInline(line);
    constexpr int kWidth = 140;
    if (line.size() <= kWidth)
        return line;
    const int hit = line.indexOf(needle, 0, Qt::CaseInsensitive);
    const int from = qBound(0, hit - kWidth / 3, int(line.size()) - kWidth);
    return (from > 0 ? QString(QChar(0x2026)) : QString()) + line.mid(from, kWidth).trimmed() +
           (from + kWidth < line.size() ? QString(QChar(0x2026)) : QString());
}

}

QString DocsLibrary::bundledRoot()
{
    return QStringLiteral(":/forkmesh-market/packages/docs/repository/www/docs");
}

const QList<DocsArea> &DocsLibrary::areas()
{
    static const QList<DocsArea> list = {
        {QStringLiteral("guides"), QStringLiteral("Guides"),
         QStringLiteral("Getting started, the product promise, business model, identities, hosted "
                        "repositories and imports.")},
        {QStringLiteral("design"), QStringLiteral("Design"),
         QStringLiteral("Protocol and federation design decisions.")},
        {QStringLiteral("engineering"), QStringLiteral("Engineering"),
         QStringLiteral("Quality gates, performance budgets and the definition of done.")},
        {QStringLiteral("security"), QStringLiteral("Security"),
         QStringLiteral("Security boundaries and local integration contracts.")},
        {QStringLiteral("operations"), QStringLiteral("Operations"),
         QStringLiteral("Deployment, recovery, gateways, headless nodes and service runbooks.")},
        {QStringLiteral("mobile"), QStringLiteral("Mobile"),
         QStringLiteral("Mobile node contract and the owned Git engine.")},
        {QStringLiteral("strategy"), QStringLiteral("Strategy & research"),
         QStringLiteral("Launch readiness, weekly briefs and pricing research.")},
        {QStringLiteral("superpowers"), QStringLiteral("Plans & specs"),
         QStringLiteral("Implementation plans and design specs for shipped features.")},
        {QStringLiteral("schemas"), QStringLiteral("Schemas & data"),
         QStringLiteral("Machine-readable configuration and protocol schemas served verbatim.")},
    };
    return list;
}

const DocsArea *DocsLibrary::area(const QString &key)
{
    for (const DocsArea &entry : areas()) {
        if (entry.key == key)
            return &entry;
    }
    return nullptr;
}

QString DocsLibrary::areaFor(const QString &path)
{
    if (path.endsWith(QLatin1String(".json")))
        return QStringLiteral("schemas");
    const int slash = path.indexOf(QLatin1Char('/'));
    if (slash < 0)
        return QStringLiteral("guides");
    return directoryAreas().value(path.left(slash), QStringLiteral("guides"));
}

QString DocsLibrary::webUrl(const QString &path)
{
    if (!path.endsWith(QLatin1String(".md")))
        return kWebOrigin + QStringLiteral("/docs/") + path;
    if (path == QLatin1String("README.md"))
        return kWebOrigin + QStringLiteral("/docs/library/");
    QStringList parts = path.split(QLatin1Char('/'));
    if (parts.last() == QLatin1String("README.md"))
        parts.removeLast();
    else
        parts.last().chop(3);
    if (parts.size() == 1 && kReservedSlugs.contains(parts.first()))
        parts.append(QStringLiteral("reference"));
    return kWebOrigin + QStringLiteral("/docs/") + parts.join(QLatin1Char('/')) +
           QLatin1Char('/');
}

QString DocsLibrary::sourceUrl(const QString &repositoryPath)
{
    return kSourceTree + repositoryPath;
}

QString DocsLibrary::titleOf(const QString &markdown)
{
    static const QRegularExpression heading(QStringLiteral("^#\\s+(.+?)\\s*#*\\s*$"));
    for (const QString &line : proseLines(markdown)) {
        const QRegularExpressionMatch match = heading.match(line.trimmed());
        if (match.hasMatch())
            return stripInline(match.captured(1));
    }
    return {};
}

QString DocsLibrary::descriptionOf(const QString &markdown)
{
    static const QRegularExpression block(QStringLiteral(
        "^(#{1,6}(\\s|$)|\\||[-*+]\\s|\\d+[.)]\\s|</?[A-Za-z][A-Za-z0-9]*(\\s|/?>|$)|---|===|\\[[^\\]]+\\]:)"));
    static const QRegularExpression quote(QStringLiteral("^(>\\s?)+"));
    static const QRegularExpression comment(QStringLiteral("<!--.*?-->"),
                                            QRegularExpression::DotMatchesEverythingOption);
    QStringList paragraph;
    bool quoted = false;
    for (const QString &line : proseLines(QString(markdown).remove(comment))) {
        QString trimmed = line.trimmed();
        const bool quoteLine = trimmed.startsWith(QLatin1Char('>'));
        if (!paragraph.isEmpty() && quoteLine != quoted)
            break;
        if (quoteLine) {
            trimmed.remove(quote);
            trimmed = trimmed.trimmed();
        }
        quoted = quoteLine;
        if (trimmed.isEmpty()) {
            if (!paragraph.isEmpty())
                break;
            continue;
        }
        if (block.match(trimmed).hasMatch()) {
            if (!paragraph.isEmpty())
                break;
            continue;
        }
        paragraph.append(trimmed);
    }
    return truncated(stripInline(paragraph.join(QLatin1Char(' '))), kDescriptionLimit);
}

QStringList DocsLibrary::linkTargets(const QString &markdown)
{
    static const QRegularExpression link(
        QStringLiteral("\\]\\(\\s*<?([^)\\s>]+)>?(?:\\s+\"[^\"]*\")?\\s*\\)"));
    static const QRegularExpression href(QStringLiteral("href=\"([^\"]+)\""));
    QStringList targets;
    for (const QString &line : proseLines(markdown)) {
        for (const QRegularExpression *pattern : {&link, &href}) {
            QRegularExpressionMatchIterator it = pattern->globalMatch(line);
            while (it.hasNext())
                targets.append(it.next().captured(1));
        }
    }
    return targets;
}

int DocsLibrary::readingMinutes(int words)
{
    return words > 0 ? qMax(1, int(std::lround(words / 220.0))) : 0;
}

bool DocsLibrary::load(const QString &root)
{
    m_pages.clear();
    m_index.clear();
    const QString base = QDir(root).absolutePath();
    if (!QDir(base).exists())
        return false;
    static const QRegularExpression word(QStringLiteral("\\S+"));
    QDirIterator it(base, {QStringLiteral("*.md"), QStringLiteral("*.json")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        if (!file.startsWith(base + QLatin1Char('/')))
            continue;
        const QString path = file.mid(base.size() + 1);
        if (path.startsWith(QLatin1Char('.')) || path.contains(QLatin1String("/.")))
            continue;
        QFile handle(file);
        if (!handle.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = handle.readAll();
        DocsPage page;
        page.path = path;
        page.area = areaFor(path);
        if (path.endsWith(QLatin1String(".json"))) {
            page.asset = true;
            page.title = assetTitle(path);
            page.description = assetDescription(bytes);
            page.markdown = QStringLiteral("# %1\n\n%2\n\n```json\n%3\n```\n")
                                .arg(page.title, page.description,
                                     QString::fromUtf8(bytes).trimmed());
        } else {
            page.markdown = QString::fromUtf8(bytes);
            page.title = titleOf(page.markdown);
            if (page.title.isEmpty())
                page.title = path.section(QLatin1Char('/'), -1).chopped(3);
            page.description = descriptionOf(page.markdown);
        }
        page.searchText = page.markdown.toLower();
        for (QRegularExpressionMatchIterator words = word.globalMatch(page.markdown);
             words.hasNext(); words.next())
            ++page.words;
        m_pages.append(page);
    }

    QHash<QString, int> areaOrder;
    for (int i = 0; i < areas().size(); ++i)
        areaOrder.insert(areas().at(i).key, i);
    std::sort(m_pages.begin(), m_pages.end(), [&areaOrder](const DocsPage &a, const DocsPage &b) {
        const int areaA = areaOrder.value(a.area);
        const int areaB = areaOrder.value(b.area);
        if (areaA != areaB)
            return areaA < areaB;
        if (a.area == QLatin1String("guides")) {
            const int rankA = kGuideOrder.indexOf(a.path) < 0 ? kGuideOrder.size()
                                                               : kGuideOrder.indexOf(a.path);
            const int rankB = kGuideOrder.indexOf(b.path) < 0 ? kGuideOrder.size()
                                                               : kGuideOrder.indexOf(b.path);
            if (rankA != rankB)
                return rankA < rankB;
            if (rankA == kGuideOrder.size() && a.path != b.path)
                return a.path < b.path;
        }
        const int title = a.title.compare(b.title, Qt::CaseInsensitive);
        return title != 0 ? title < 0 : a.path < b.path;
    });
    for (int i = 0; i < m_pages.size(); ++i)
        m_index.insert(m_pages.at(i).path, i);

    for (DocsPage &page : m_pages) {
        if (page.asset)
            continue;
        for (const QString &target : linkTargets(page.markdown)) {
            const DocsTarget resolved = resolve(page.path, target);
            if (resolved.kind == DocsTarget::Kind::Page && resolved.path != page.path &&
                !page.links.contains(resolved.path))
                page.links.append(resolved.path);
        }
    }
    for (const DocsPage &page : std::as_const(m_pages)) {
        for (const QString &target : page.links) {
            DocsPage &linked = m_pages[m_index.value(target)];
            if (!linked.backlinks.contains(page.path))
                linked.backlinks.append(page.path);
        }
    }
    return !m_pages.isEmpty();
}

const DocsPage *DocsLibrary::page(const QString &path) const
{
    const auto it = m_index.constFind(path);
    return it == m_index.constEnd() ? nullptr : &m_pages.at(it.value());
}

int DocsLibrary::indexOf(const QString &path) const
{
    return m_index.value(path, -1);
}

QList<const DocsPage *> DocsLibrary::pagesIn(const QString &area) const
{
    QList<const DocsPage *> out;
    for (const DocsPage &page : m_pages) {
        if (page.area == area)
            out.append(&page);
    }
    return out;
}

QString DocsLibrary::homePath() const
{
    if (m_index.contains(QStringLiteral("README.md")))
        return QStringLiteral("README.md");
    return m_pages.isEmpty() ? QString() : m_pages.first().path;
}

QString DocsLibrary::previousPath(const QString &path) const
{
    const int at = indexOf(path);
    return at > 0 ? m_pages.at(at - 1).path : QString();
}

QString DocsLibrary::nextPath(const QString &path) const
{
    const int at = indexOf(path);
    return at >= 0 && at + 1 < m_pages.size() ? m_pages.at(at + 1).path : QString();
}

QList<DocsHit> DocsLibrary::search(const QString &query, int limit) const
{
    QList<DocsHit> hits;
    const QString phrase = query.simplified().toLower();
    const QStringList tokens = phrase.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty() || limit <= 0)
        return hits;
    for (const DocsPage &page : m_pages) {
        const QString title = page.title.toLower();
        const QString description = page.description.toLower();
        const QString path = page.path.toLower();
        int score = title.contains(phrase) ? 200 : 0;
        QString bodyToken;
        bool matched = true;
        for (const QString &token : tokens) {
            const int body = qMin(10, int(page.searchText.count(token)));
            if (title.contains(token))
                score += title.startsWith(token) ? 150 : 100;
            else if (description.contains(token))
                score += 25;
            else if (path.contains(token))
                score += 15;
            else if (body == 0) {
                matched = false;
                break;
            }
            if (body > 0 && bodyToken.isEmpty())
                bodyToken = token;
            score += body;
        }
        if (!matched)
            continue;
        QString snippet = page.searchText.contains(phrase) ? snippetFor(page.markdown, phrase)
                                                           : QString();
        if (snippet.isEmpty() && !bodyToken.isEmpty())
            snippet = snippetFor(page.markdown, bodyToken);
        if (snippet.isEmpty() || snippet == page.title)
            snippet = page.description;
        hits.append({page.path, snippet, score});
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const DocsHit &a, const DocsHit &b) { return a.score > b.score; });
    if (hits.size() > limit)
        hits.resize(limit);
    return hits;
}

QString DocsLibrary::pagePathFor(const QString &repositoryPath) const
{
    if (repositoryPath == QLatin1String("www/docs"))
        return m_index.contains(QStringLiteral("README.md")) ? QStringLiteral("README.md")
                                                              : QString();
    if (!repositoryPath.startsWith(kRepositoryPrefix))
        return {};
    const QString inner = repositoryPath.mid(kRepositoryPrefix.size());
    if (m_index.contains(inner))
        return inner;
    const QString readme = inner + QStringLiteral("/README.md");
    return m_index.contains(readme) ? readme : QString();
}

DocsTarget DocsLibrary::resolve(const QString &fromPath, const QString &href) const
{
    DocsTarget target;
    const QString source = href.trimmed();
    if (source.isEmpty())
        return target;
    if (source.startsWith(QLatin1Char('#'))) {
        target.kind = DocsTarget::Kind::Anchor;
        target.path = fromPath;
        target.fragment = QUrl::fromPercentEncoding(source.mid(1).toUtf8());
        return target;
    }
    if (source.startsWith(QLatin1String("//"))) {
        target.kind = DocsTarget::Kind::External;
        target.url = QUrl(QStringLiteral("https:") + source);
        return target;
    }
    if (source.startsWith(QLatin1Char('/'))) {
        target.kind = DocsTarget::Kind::External;
        target.url = QUrl(kWebOrigin + source);
        return target;
    }
    const std::optional<MarkdownRender::RepoLink> link =
        MarkdownRender::resolveRepoLink(kRepositoryPrefix + fromPath, source);
    if (!link) {
        const QUrl url(source);
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https") ||
            scheme == QLatin1String("mailto")) {
            target.kind = DocsTarget::Kind::External;
            target.url = url;
        }
        return target;
    }
    const QString page = pagePathFor(link->path);
    if (!page.isEmpty()) {
        target.kind = DocsTarget::Kind::Page;
        target.path = page;
        target.fragment = link->fragment;
        return target;
    }
    const QString suffix = link->fragment.isEmpty() ? QString() : QLatin1Char('#') + link->fragment;
    target.kind = DocsTarget::Kind::External;
    if (link->path.startsWith(QLatin1String("www/public/")))
        target.url = QUrl(kWebOrigin + QLatin1Char('/') +
                          link->path.mid(QStringLiteral("www/public/").size()) + suffix);
    else
        target.url = QUrl(sourceUrl(link->path) + suffix);
    return target;
}

}
