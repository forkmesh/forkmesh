#pragma once

// Preparing README-shaped markdown for QTextDocument::setMarkdown.
//
// Real READMEs ship raw HTML: a centred logo, a badge/link row, <br> breaks,
// a <details> block. Qt's markdown importer cannot render any of it, and it
// does not fail gracefully. md4c reports the HTML verbatim, so a block of it
// is folded into whatever block came before (the forkmesh header row landed
// *inside* the <h1>), and one unclosed void tag - `<img src="logo.png"
// width="120">`, exactly what the forkmesh README has - puts the importer in a
// state where it swallows every following plain-text run. The repo overview
// then showed a heading, eight empty bullets, and floating code blocks with no
// prose between them, because only headings, links and code survived.
//
// Flattening that HTML into the markdown it stands for, before the document
// sees it, keeps the content *and* stops the poisoning. Fenced code and inline
// code spans are copied through untouched so documentation about HTML still
// reads as written.
//
// Deliberately Qt Core only (QString/QRegularExpression) so forkmesh-tests can
// cover it; the widget side lives in MainWindowRepoDetail.cpp.

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace MarkdownRender {

// Width declared by an <img width="120"> survives as a "#w=120" fragment on
// the image URL: markdown has no size syntax, and the reader strips the hint
// again when it sizes the image.
inline const QString &imageWidthHint()
{
    static const QString hint = QStringLiteral("#w=");
    return hint;
}

namespace Detail {

inline QString tagAttribute(const QString &tag, const QString &name)
{
    const QRegularExpression re(
        QStringLiteral("\\b%1\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))")
            .arg(name),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(tag);
    if (!m.hasMatch())
        return QString();
    for (int i = 1; i <= 3; ++i) {
        if (m.capturedStart(i) >= 0)
            return m.captured(i);
    }
    return QString();
}

inline QString decodeEntities(QString text)
{
    static const QRegularExpression numeric(
        QStringLiteral("&#(x?)([0-9a-fA-F]+);"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(text.size());
    int last = 0;
    QRegularExpressionMatchIterator it = numeric.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const bool hex = !m.captured(1).isEmpty();
        bool ok = false;
        const uint code = m.captured(2).toUInt(&ok, hex ? 16 : 10);
        if (!ok || code == 0 || code > 0x10FFFF)
            continue;
        const char32_t point = static_cast<char32_t>(code);
        out += text.mid(last, m.capturedStart() - last);
        out += QString::fromUcs4(&point, 1);
        last = m.capturedEnd();
    }
    out += text.mid(last);
    text = out;

    // &amp; is decoded last so "&amp;lt;" stays the literal text "&lt;".
    static const QList<QPair<QString, QString>> named = {
        {QStringLiteral("&nbsp;"), QStringLiteral(" ")},
        {QStringLiteral("&lt;"), QStringLiteral("<")},
        {QStringLiteral("&gt;"), QStringLiteral(">")},
        {QStringLiteral("&quot;"), QStringLiteral("\"")},
        {QStringLiteral("&apos;"), QStringLiteral("'")},
        {QStringLiteral("&middot;"), QStringLiteral("\u00B7")},
        {QStringLiteral("&bull;"), QStringLiteral("\u2022")},
        {QStringLiteral("&mdash;"), QStringLiteral("\u2014")},
        {QStringLiteral("&ndash;"), QStringLiteral("\u2013")},
        {QStringLiteral("&hellip;"), QStringLiteral("\u2026")},
        {QStringLiteral("&rarr;"), QStringLiteral("\u2192")},
        {QStringLiteral("&copy;"), QStringLiteral("\u00A9")},
        {QStringLiteral("&amp;"), QStringLiteral("&")},
    };
    for (const auto &entity : named)
        text.replace(entity.first, entity.second, Qt::CaseInsensitive);
    return text;
}

// A "<" the tag pass left behind is literal text - an entity that decoded to
// one, or markup we do not recognise. Escaping it keeps md4c from starting a
// fresh HTML run over it. Autolinks (<https://…>, <a@b.c>) stay untouched.
inline QString escapeStrayAngleBrackets(QString text)
{
    static const QRegularExpression stray(
        QStringLiteral("<(?![a-zA-Z][a-zA-Z0-9+.-]*[:@])(?=[/!?a-zA-Z])"));
    return text.replace(stray, QStringLiteral("\\<"));
}

inline QString flattenTags(QString segment)
{
    static const QRegularExpression imageTag(QStringLiteral("<img\\b[^>]*>"),
                                             QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(segment.size());
    int last = 0;
    QRegularExpressionMatchIterator images = imageTag.globalMatch(segment);
    while (images.hasNext()) {
        const QRegularExpressionMatch m = images.next();
        const QString source = tagAttribute(m.captured(), QStringLiteral("src"));
        out += segment.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        if (source.isEmpty())
            continue;
        const QString alt = tagAttribute(m.captured(), QStringLiteral("alt"));
        bool widthOk = false;
        const int width =
            tagAttribute(m.captured(), QStringLiteral("width")).toInt(&widthOk);
        const bool hintable = widthOk && width > 0 && !source.contains(QLatin1Char('#'));
        out += QStringLiteral("![%1](%2%3)")
                   .arg(alt, source,
                        hintable ? imageWidthHint() + QString::number(width) : QString());
    }
    out += segment.mid(last);
    segment = out;

    static const QRegularExpression anchor(
        QStringLiteral("<a\\b[^>]*\\bhref\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))"
                       "[^>]*>(.*?)</a\\s*>"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator links = anchor.globalMatch(segment);
    out.clear();
    last = 0;
    while (links.hasNext()) {
        const QRegularExpressionMatch m = links.next();
        QString href;
        for (int i = 1; i <= 3 && href.isEmpty(); ++i) {
            if (m.capturedStart(i) >= 0)
                href = m.captured(i);
        }
        const QString label = m.captured(4).trimmed();
        out += segment.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        if (label.isEmpty())
            continue;
        out += href.isEmpty() ? label : QStringLiteral("[%1](%2)").arg(label, href);
    }
    out += segment.mid(last);
    segment = out;

    static const QList<QPair<QRegularExpression, QString>> simple = {
        {QRegularExpression(QStringLiteral("</?(?:strong|b)\\s*>"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("**")},
        {QRegularExpression(QStringLiteral("</?(?:em|i)\\s*>"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("*")},
        {QRegularExpression(QStringLiteral("</?(?:code|tt)\\s*>"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("`")},
        {QRegularExpression(QStringLiteral("</?(?:del|s|strike)\\s*>"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("~~")},
        {QRegularExpression(QStringLiteral("<br\\s*/?>"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("  \n")},
        {QRegularExpression(QStringLiteral("<hr\\s*/?>"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("\n\n---\n\n")},
        {QRegularExpression(QStringLiteral("<li\\b[^>]*>"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("\n- ")},
        {QRegularExpression(QStringLiteral("</?(?:p|div|center|section|summary|"
                                           "details|ul|ol|table|tr|blockquote)\\b[^>]*>"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("\n\n")},
        {QRegularExpression(QStringLiteral("</?(?:td|th)\\b[^>]*>"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral(" ")},
    };
    for (const auto &rule : simple)
        segment.replace(rule.first, rule.second);

    static const QRegularExpression heading(QStringLiteral("<h([1-6])\\b[^>]*>"),
                                            QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator headings = heading.globalMatch(segment);
    out.clear();
    last = 0;
    while (headings.hasNext()) {
        const QRegularExpressionMatch m = headings.next();
        out += segment.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        out += QStringLiteral("\n\n") +
               QString(m.captured(1).toInt(), QLatin1Char('#')) + QLatin1Char(' ');
    }
    out += segment.mid(last);
    segment = out;

    // Whatever markup is left is not something we can render; drop the tags
    // and keep the text they wrapped.
    static const QRegularExpression anyTag(
        QStringLiteral("</?[a-zA-Z][a-zA-Z0-9-]*(?:\\s[^>]*)?/?>"),
        QRegularExpression::CaseInsensitiveOption);
    segment.remove(anyTag);
    return escapeStrayAngleBrackets(decodeEntities(segment));
}

// Flattens one markdown line, leaving `inline code spans` exactly as written.
inline QString flattenLine(const QString &line)
{
    QString out;
    out.reserve(line.size());
    QString plain;
    int i = 0;
    while (i < line.size()) {
        if (line.at(i) != QLatin1Char('`')) {
            plain += line.at(i);
            ++i;
            continue;
        }
        int ticks = 0;
        while (i + ticks < line.size() && line.at(i + ticks) == QLatin1Char('`'))
            ++ticks;
        const QString fence(ticks, QLatin1Char('`'));
        const int close = line.indexOf(fence, i + ticks);
        if (close < 0) { // unbalanced: not a code span
            plain += fence;
            i += ticks;
            continue;
        }
        out += flattenTags(plain);
        plain.clear();
        out += line.mid(i, close + ticks - i);
        i = close + ticks;
    }
    return out + flattenTags(plain);
}

} // namespace Detail

// Markdown with its raw HTML rewritten as markdown. Fenced code blocks and
// inline code spans are preserved byte for byte.
inline QString withHtmlFlattened(const QString &markdown)
{
    static const QRegularExpression comment(QStringLiteral("<!--.*?-->"),
                                            QRegularExpression::DotMatchesEverythingOption);
    QString source = markdown;
    source.remove(comment);

    const QStringList lines = source.split(QLatin1Char('\n'));
    QStringList kept;
    kept.reserve(lines.size());
    QString fence;
    // Dropping a <p>/<div> wrapper leaves the blank line it stood on behind,
    // so runs of them are collapsed - but only outside fenced code, where
    // blank lines are content.
    const auto appendOutsideFence = [&kept](const QString &line) {
        if (line.trimmed().isEmpty() && (kept.isEmpty() || kept.last().trimmed().isEmpty()))
            return;
        kept.append(line);
    };
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!fence.isEmpty()) {
            kept.append(line);
            if (trimmed.startsWith(fence))
                fence.clear();
            continue;
        }
        if (trimmed.startsWith(QLatin1String("```")) ||
            trimmed.startsWith(QLatin1String("~~~"))) {
            fence = trimmed.left(3);
            kept.append(line);
            continue;
        }
        const QStringList produced =
            Detail::flattenLine(line).split(QLatin1Char('\n'));
        for (const QString &out : produced)
            appendOutsideFence(out);
    }
    while (!kept.isEmpty() && kept.constFirst().trimmed().isEmpty())
        kept.removeFirst();
    return kept.join(QLatin1Char('\n'));
}

// GitHub's heading anchor: "## 2. Add an independent mirror" is linked to as
// "#2-add-an-independent-mirror".
inline QString headingAnchor(const QString &heading)
{
    QString out;
    const QString lowered = heading.trimmed().toLower();
    for (const QChar ch : lowered) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-'))
            out += ch;
        else if (ch.isSpace())
            out += QLatin1Char('-');
    }
    return out;
}

struct RepoLink {
    QString path;           // repository-relative; empty is the repository root
    QString fragment;       // heading anchor, without the '#'
    bool directory = false; // written with a trailing '/'
};

// A relative markdown link inside the repository file `basePath` (such as
// "www/docs/intro.md") names another repository path, resolved against that
// file's directory. Anything else - a URL with a scheme, an absolute or
// protocol-relative path, a same-document "#anchor" - is not a repository
// link and yields nullopt.
inline std::optional<RepoLink> resolveRepoLink(const QString &basePath, const QString &href)
{
    const QString source = href.trimmed();
    static const QRegularExpression scheme(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*:"));
    if (source.isEmpty() || source.startsWith(QLatin1Char('#')) ||
        source.startsWith(QLatin1Char('/')) || scheme.match(source).hasMatch())
        return std::nullopt;

    RepoLink link;
    QString target = source;
    const int hash = target.indexOf(QLatin1Char('#'));
    if (hash >= 0) {
        link.fragment = QUrl::fromPercentEncoding(target.mid(hash + 1).toUtf8());
        target.truncate(hash);
    }
    const int query = target.indexOf(QLatin1Char('?'));
    if (query >= 0)
        target.truncate(query);
    target = QUrl::fromPercentEncoding(target.toUtf8());
    link.directory = target.isEmpty() || target.endsWith(QLatin1Char('/'));

    const int slash = basePath.lastIndexOf(QLatin1Char('/'));
    QStringList parts = slash >= 0 ? basePath.left(slash).split(QLatin1Char('/'))
                                   : QStringList();
    for (const QString &part : target.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..")) {
            if (!parts.isEmpty())
                parts.removeLast();
        } else {
            parts.append(part);
        }
    }
    parts.removeAll(QString());
    link.path = parts.join(QLatin1Char('/'));
    if (link.path.isEmpty())
        link.directory = true;
    return link;
}

} // namespace MarkdownRender
