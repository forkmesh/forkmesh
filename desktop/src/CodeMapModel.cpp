#include "CodeMapModel.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace forkmesh::codemap {

namespace {

// Bytes. Keeps an empty or tiny file a visible speck instead of a zero-area
// cell nobody can find.
constexpr double kMinWeight = 48;
constexpr int kMaxLines = 20000;
constexpr int kMaxLineChars = 240;

const QHash<QString, QString> &languageByExtension()
{
    static const QHash<QString, QString> map = {
        {"js", "JavaScript"}, {"mjs", "JavaScript"}, {"cjs", "JavaScript"}, {"jsx", "JavaScript"},
        {"ts", "TypeScript"}, {"tsx", "TypeScript"}, {"py", "Python"}, {"pyi", "Python"},
        {"cpp", "C++"}, {"cc", "C++"}, {"cxx", "C++"}, {"hpp", "C++"}, {"hh", "C++"}, {"h", "C++"},
        {"c", "C"}, {"go", "Go"}, {"rs", "Rust"}, {"dart", "Dart"}, {"kt", "Kotlin"},
        {"kts", "Kotlin"}, {"java", "Java"}, {"swift", "Swift"}, {"rb", "Ruby"}, {"php", "PHP"},
        {"cs", "C#"}, {"lua", "Lua"}, {"sql", "SQL"}, {"html", "HTML"}, {"htm", "HTML"},
        {"css", "CSS"}, {"scss", "CSS"}, {"less", "CSS"}, {"qss", "CSS"}, {"md", "Markdown"},
        {"markdown", "Markdown"}, {"rst", "Markdown"}, {"txt", "Text"}, {"json", "JSON"},
        {"yml", "YAML"}, {"yaml", "YAML"}, {"toml", "TOML"}, {"xml", "XML"}, {"qrc", "XML"},
        {"ui", "XML"}, {"sh", "Shell"}, {"bash", "Shell"}, {"zsh", "Shell"}, {"ps1", "Shell"},
        {"bat", "Shell"}, {"cmake", "CMake"}, {"svg", "SVG"}, {"patch", "Patch"},
        {"diff", "Patch"}, {"png", "Image"}, {"jpg", "Image"}, {"jpeg", "Image"},
        {"gif", "Image"}, {"webp", "Image"}, {"ico", "Image"}, {"avif", "Image"},
        {"woff", "Font"}, {"woff2", "Font"}, {"ttf", "Font"}, {"otf", "Font"},
    };
    return map;
}

const QSet<QString> &binaryExtensions()
{
    static const QSet<QString> set = {
        "png", "jpg", "jpeg", "gif", "webp", "ico", "avif", "bmp", "icns", "woff", "woff2",
        "ttf", "otf", "eot", "zip", "gz", "tgz", "xz", "bz2", "7z", "tar", "jar", "apk", "aab",
        "deb", "rpm", "dmg", "exe", "dll", "so", "dylib", "a", "o", "bin", "dat", "db",
        "sqlite", "pdf", "mp3", "mp4", "webm", "ogg", "wav", "mov", "glb", "gltf", "wasm",
        "pyc", "class", "keystore", "jks", "age", "pack", "idx",
    };
    return set;
}

const QSet<QString> &hashCommentLanguages()
{
    static const QSet<QString> set = {"Python", "Shell", "YAML", "TOML", "Ruby", "CMake"};
    return set;
}

const QSet<QString> &dashCommentLanguages()
{
    static const QSet<QString> set = {"SQL", "Lua"};
    return set;
}

const QSet<QString> &noSlashCommentLanguages()
{
    static const QSet<QString> set = {
        "Python", "Shell", "YAML", "TOML", "Ruby", "CMake", "Markdown", "Text",
        "HTML", "XML", "SVG", "JSON", "Patch", "SQL", "Lua",
    };
    return set;
}

const QSet<QString> &proseLanguages()
{
    static const QSet<QString> set = {"Markdown", "Text", "Patch"};
    return set;
}

const QSet<QString> &keywords()
{
    static const QSet<QString> set = [] {
        const QString words = QStringLiteral(
            "abstract and as assert async auto await bool break case catch char class const "
            "constexpr continue def default defer del delete do double dynamic elif else enum "
            "except explicit export extends extern false final finally float fn for from func "
            "function global go if impl implements import in inline instanceof int interface is "
            "lambda let long match mod mut namespace new nil none None not null nullptr of "
            "operator or override package pass private protected pub public raise return self "
            "short signed sizeof static struct super switch template this throw true True False "
            "try type typedef typename typeof union unsigned use using var virtual void volatile "
            "while with yield select chan range map string");
        const QStringList list = words.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        return QSet<QString>(list.begin(), list.end());
    }();
    return set;
}

const QRegularExpression &tokenPattern()
{
    static const QRegularExpression pattern(QStringLiteral(
        R"RX((\s+)|(//.*|/\*.*?(?:\*/|$))|(#.*)|(--.*)|(<!--.*?(?:-->|$))|("""(?:.*?)(?:"""|$)|"(?:[^"\\]|\\.)*"?|'(?:[^'\\]|\\.)*'?|`(?:[^`\\]|\\.)*`?)|(\d[\w.]*)|([A-Za-z_$][\w$]*)|(.))RX"));
    return pattern;
}

int leadingSpaces(const QString &line)
{
    int lead = 0;
    while (lead < line.size() && line.at(lead).isSpace())
        ++lead;
    return lead;
}

void squarify(QVector<Node> &nodes, Node &parent, double x, double y, double w, double h)
{
    parent.strips.clear();
    const QVector<int> &items = parent.children;
    double total = 0;
    for (int index : items)
        total += nodes[index].weight;
    if (!(total > 0) || !(w > 0) || !(h > 0)) {
        for (int index : items)
            nodes[index].rect = QRectF(x, y, 0, 0);
        return;
    }
    const double scale = (w * h) / total;
    double rx = x, ry = y, rw = w, rh = h;
    int i = 0;
    while (i < items.size()) {
        const double shortSide = std::max(std::min(rw, rh), 1e-9);
        const double s2 = shortSide * shortSide;
        double sum = 0, minArea = 1e300, maxArea = 0, best = 1e300;
        int j = i;
        while (j < items.size()) {
            const double area = nodes[items[j]].weight * scale;
            const double nextSum = sum + area;
            const double nextMin = std::min(minArea, area);
            const double nextMax = std::max(maxArea, area);
            const double worst = std::max((s2 * nextMax) / (nextSum * nextSum),
                                          (nextSum * nextSum) / (s2 * nextMin));
            if (j > i && worst > best)
                break;
            best = worst;
            sum = nextSum;
            minArea = nextMin;
            maxArea = nextMax;
            ++j;
        }
        Node::Strip strip;
        strip.first = i;
        strip.count = j - i;
        if (rw >= rh) {
            const double stripW = std::min(rw, sum / std::max(rh, 1e-9));
            double cy = ry;
            for (int k = i; k < j; ++k) {
                Node &item = nodes[items[k]];
                const double hh = stripW > 0 ? (item.weight * scale) / stripW : 0;
                item.rect = QRectF(rx, cy, stripW, hh);
                cy += hh;
            }
            rx += stripW;
            rw -= stripW;
            strip.vertical = true;
            strip.edge = rx;
        } else {
            const double stripH = std::min(rh, sum / std::max(rw, 1e-9));
            double cx = rx;
            for (int k = i; k < j; ++k) {
                Node &item = nodes[items[k]];
                const double ww = stripH > 0 ? (item.weight * scale) / stripH : 0;
                item.rect = QRectF(cx, ry, ww, stripH);
                cx += ww;
            }
            ry += stripH;
            rh -= stripH;
            strip.vertical = false;
            strip.edge = ry;
        }
        parent.strips.append(strip);
        i = j;
    }
}

// A directory keeps a label band on top and a thin frame, both proportional to
// its own size so they stay in scale at every zoom level; files keep a smaller
// name band.
void layoutNode(QVector<Node> &nodes, int index)
{
    Node &node = nodes[index];
    const double shortSide = std::min(node.rect.width(), node.rect.height());
    if (node.type != Node::Directory || node.children.isEmpty()) {
        node.band = node.type == Node::File
            ? std::min({node.rect.height() * 0.14, shortSide * 0.07, 7.0})
            : 0.0;
        return;
    }
    const double pad = std::min(shortSide * 0.012, 3.0);
    node.band = node.depth == 0
        ? 0.0
        : std::min({14.0, shortSide * 0.07, node.rect.height() * 0.25});
    squarify(nodes, node, node.rect.x() + pad, node.rect.y() + pad + node.band,
             std::max(0.0, node.rect.width() - pad * 2),
             std::max(0.0, node.rect.height() - pad * 2 - node.band));
    const QVector<int> children = node.children; // `node` may not dangle, but be explicit
    for (int child : children)
        layoutNode(nodes, child);
}

} // namespace

QString languageOf(const QString &fileName)
{
    const QString base = fileName.section(QLatin1Char('/'), -1).toLower();
    if (base == QLatin1String("cmakelists.txt"))
        return QStringLiteral("CMake");
    if (base == QLatin1String("makefile") || base == QLatin1String("dockerfile"))
        return QStringLiteral("Shell");
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return QStringLiteral("Other");
    const QString ext = base.mid(dot + 1);
    const auto found = languageByExtension().constFind(ext);
    if (found != languageByExtension().constEnd())
        return found.value();
    return ext.size() <= 5 ? QLatin1Char('.') + ext : QStringLiteral("Other");
}

bool isBinaryName(const QString &fileName)
{
    const QString lower = fileName.toLower();
    const int dot = lower.lastIndexOf(QLatin1Char('.'));
    return dot > 0 && binaryExtensions().contains(lower.mid(dot + 1));
}

// Kept in step with codemap.js (IMAGE_PREVIEW_EXT, FONT_PREVIEW_EXT,
// FILE_CATEGORIES).
QString extensionOf(const QString &fileName)
{
    const QString base = fileName.section(QLatin1Char('/'), -1).toLower();
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? base.mid(dot + 1) : QString();
}

Preview previewKindOf(const QString &fileName)
{
    static const QSet<QString> images = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"),
        QStringLiteral("webp"), QStringLiteral("avif"), QStringLiteral("bmp"), QStringLiteral("ico"),
        QStringLiteral("svg")};
    static const QSet<QString> fonts = {
        QStringLiteral("ttf"), QStringLiteral("otf"), QStringLiteral("woff"), QStringLiteral("woff2")};
    const QString ext = extensionOf(fileName);
    if (images.contains(ext))
        return Preview::Image;
    if (fonts.contains(ext))
        return Preview::Font;
    return isBinaryName(fileName) ? Preview::Card : Preview::Code;
}

QString fileCategoryOf(const QString &fileName)
{
    static const QHash<QString, QString> categories = [] {
        const std::pair<const char *, const char *> table[] = {
            {"Image", "png jpg jpeg gif webp avif bmp ico svg icns tif tiff psd heic xcf"},
            {"Font", "ttf otf woff woff2 eot"},
            {"Archive", "zip gz tgz xz bz2 7z tar jar apk aab deb rpm dmg zst"},
            {"Audio", "mp3 ogg wav flac m4a opus weba aac"},
            {"Video", "mp4 webm mov mkv avi m4v"},
            {"Document", "pdf doc docx xls xlsx ppt pptx odt ods odp"},
            {"3D model", "glb gltf obj stl fbx blend"},
            {"Program", "exe dll so dylib a o bin wasm class pyc"},
            {"Data", "db sqlite dat pack idx keystore jks age"},
        };
        QHash<QString, QString> out;
        for (const auto &[category, extensions] : table) {
            for (const QString &ext : QString::fromLatin1(extensions).split(QLatin1Char(' ')))
                out.insert(ext, QString::fromLatin1(category));
        }
        return out;
    }();
    return categories.value(extensionOf(fileName), QStringLiteral("Binary"));
}

QRectF previewArea(const Node &node, double aspect)
{
    const double inset = std::min(node.rect.width(), node.rect.height()) * 0.035;
    const double cw = std::max(0.0, node.rect.width() - inset * 2);
    const double ch = std::max(0.0, node.rect.height() - node.band - inset * 2);
    if (!(aspect > 0) || !(cw > 0) || !(ch > 0))
        return QRectF(inset, node.band + inset, cw, ch);
    double w = cw, h = cw / aspect;
    if (h > ch) {
        h = ch;
        w = ch * aspect;
    }
    return QRectF(inset + (cw - w) / 2, node.band + inset + (ch - h) / 2, w, h);
}

QHash<QString, QByteArray> parseCatFileBatch(const QByteArray &output, const QStringList &paths)
{
    QHash<QString, QByteArray> blobs;
    qsizetype at = 0;
    for (const QString &path : paths) {
        const qsizetype newline = output.indexOf('\n', at);
        if (newline < 0)
            break;
        const QByteArray header = output.mid(at, newline - at);
        at = newline + 1;
        // "<oid> <type> <size>" on success; "<name> missing" otherwise, and a
        // name may itself contain spaces, so test the ending, not the fields.
        if (header.endsWith(" missing") || header.endsWith(" ambiguous"))
            continue;
        const QList<QByteArray> fields = header.split(' ');
        if (fields.size() != 3)
            break;
        bool ok = false;
        const qsizetype size = fields.at(2).toLongLong(&ok);
        if (!ok || size < 0 || at + size > output.size())
            break;
        if (fields.at(1) == "blob")
            blobs.insert(path, output.mid(at, size));
        at += size + 1; // the object is followed by one newline
    }
    return blobs;
}

Tree buildTree(const QByteArray &lsTreeOutput, const QString &rootName)
{
    QVector<Entry> entries;
    const QList<QByteArray> records = lsTreeOutput.split('\0');
    for (const QByteArray &record : records) {
        const int tab = record.indexOf('\t');
        if (tab < 0)
            continue;
        const QList<QByteArray> fields = record.left(tab).simplified().split(' ');
        if (fields.size() < 4 || fields.at(1) != "blob")
            continue;
        const QString path = QString::fromUtf8(record.mid(tab + 1));
        if (path.isEmpty())
            continue;
        const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
        entries.append({path, std::max<qint64>(0, fields.at(3).toLongLong()), languageOf(name),
                        previewKindOf(name)});
    }
    return buildTreeFromEntries(entries, rootName);
}

Tree buildTreeFromEntries(const QVector<Entry> &entries, const QString &rootName)
{
    Tree tree;
    Node root;
    root.name = rootName;
    tree.nodes.append(root);
    QHash<QString, int> directories;
    directories.insert(QString(), 0);

    auto directoryFor = [&](const QString &path) {
        // Create every missing ancestor, top down, so a parent always has a
        // smaller index than its children (the bottom-up passes rely on it).
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString walked;
        int at = 0;
        for (const QString &part : parts) {
            walked = walked.isEmpty() ? part : walked + QLatin1Char('/') + part;
            const auto found = directories.constFind(walked);
            if (found != directories.constEnd()) {
                at = found.value();
                continue;
            }
            Node dir;
            dir.name = part;
            dir.path = walked;
            dir.parent = at;
            dir.depth = tree.nodes.at(at).depth + 1;
            const int index = tree.nodes.size();
            tree.nodes.append(dir);
            tree.nodes[at].children.append(index);
            directories.insert(walked, index);
            at = index;
        }
        return at;
    };

    for (const Entry &entry : entries) {
        const QString &path = entry.path;
        if (path.isEmpty())
            continue;
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        const int parent = directoryFor(slash < 0 ? QString() : path.left(slash));
        Node file;
        file.type = Node::File;
        file.name = slash < 0 ? path : path.mid(slash + 1);
        file.path = path;
        file.size = std::max<qint64>(0, entry.size);
        file.weight = std::max<double>(double(file.size), kMinWeight);
        file.parent = parent;
        file.depth = tree.nodes.at(parent).depth + 1;
        file.language = entry.language;
        file.preview = entry.preview;
        const int index = tree.nodes.size();
        tree.nodes.append(file);
        tree.nodes[parent].children.append(index);
        ++tree.fileCount;
    }

    // Bottom up: a parent's index is always smaller than its children's.
    QVector<QHash<QString, double>> tally(tree.nodes.size());
    QHash<QString, double> totals;
    for (int i = tree.nodes.size() - 1; i >= 0; --i) {
        Node &node = tree.nodes[i];
        if (node.type == Node::File) {
            tally[i].insert(node.language, node.weight);
            totals[node.language] += node.weight;
        } else {
            QString bestLanguage;
            double bestBytes = -1;
            for (auto it = tally[i].constBegin(); it != tally[i].constEnd(); ++it) {
                if (it.value() > bestBytes
                    || (it.value() == bestBytes && it.key() < bestLanguage)) {
                    bestBytes = it.value();
                    bestLanguage = it.key();
                }
            }
            node.language = bestLanguage;
            if (node.children.isEmpty())
                node.weight = kMinWeight;
        }
        if (node.parent >= 0) {
            Node &parent = tree.nodes[node.parent];
            parent.size += node.size;
            parent.weight += node.weight;
            for (auto it = tally[i].constBegin(); it != tally[i].constEnd(); ++it)
                tally[node.parent][it.key()] += it.value();
        }
        tally[i].clear();
    }

    for (auto it = totals.constBegin(); it != totals.constEnd(); ++it) {
        if (it.key() != QLatin1String("Other"))
            tree.languages.append({it.key(), qint64(it.value())});
    }
    std::sort(tree.languages.begin(), tree.languages.end(),
              [](const QPair<QString, qint64> &a, const QPair<QString, qint64> &b) {
                  return a.second != b.second ? a.second > b.second : a.first < b.first;
              });
    QHash<QString, int> slot;
    for (int i = 0; i < tree.languages.size() && i < kHueSlots; ++i)
        slot.insert(tree.languages.at(i).first, i);
    for (Node &node : tree.nodes) {
        node.hue = slot.value(node.language, -1);
        std::sort(node.children.begin(), node.children.end(), [&tree](int a, int b) {
            const Node &left = tree.nodes.at(a);
            const Node &right = tree.nodes.at(b);
            return left.weight != right.weight ? left.weight > right.weight
                                               : left.name < right.name;
        });
    }
    return tree;
}

void layoutTree(Tree &tree, double width, double height)
{
    if (tree.nodes.isEmpty())
        return;
    tree.nodes[0].rect = QRectF(0, 0, width, height);
    layoutNode(tree.nodes, 0);
}

void elevateTree(Tree &tree, double maxHeight)
{
    if (tree.nodes.isEmpty())
        return;
    const double total = std::max<double>(double(tree.nodes.at(0).size), 1.0);
    tree.nodes[0].base = 0;
    tree.nodes[0].top = maxHeight * 0.03;
    for (int i = 1; i < tree.nodes.size(); ++i) {
        Node &node = tree.nodes[i];
        node.base = tree.nodes.at(node.parent).top;
        const bool solid = node.type == Node::Directory && !node.children.isEmpty();
        node.top = node.base
            + (solid ? std::max(maxHeight * 0.012,
                                maxHeight * std::sqrt(double(node.size) / total))
                     : maxHeight * 0.006);
    }
    for (int i = tree.nodes.size() - 1; i >= 0; --i) {
        Node &node = tree.nodes[i];
        node.subtreeTop = std::max(node.subtreeTop, node.top);
        if (node.parent >= 0) {
            Node &parent = tree.nodes[node.parent];
            parent.subtreeTop = std::max(parent.subtreeTop, node.subtreeTop);
        }
    }
}

QVector<int> backToFront(const Tree &tree, int nodeIndex, double cameraX, double cameraY)
{
    const Node &node = tree.nodes.at(nodeIndex);
    QVector<int> order;
    order.reserve(node.children.size());
    // At each cut the side the camera stands on is nearer, so it is drawn
    // after everything beyond the cut: emit far parts as they come and keep the
    // near ones for the end, innermost cut last. The same rule orders the
    // strips of a cell and the items inside one strip.
    auto emitStrip = [&](const Node::Strip &strip) {
        QVector<int> deferred;
        for (int k = 0; k < strip.count; ++k) {
            const int child = node.children.at(strip.first + k);
            const QRectF &rect = tree.nodes.at(child).rect;
            const bool last = k == strip.count - 1;
            // Items of a column are stacked top to bottom, of a row left to right.
            const bool cameraOnItemSide = strip.vertical ? cameraY < rect.bottom()
                                                         : cameraX < rect.right();
            if (last || !cameraOnItemSide)
                order.append(child);
            else
                deferred.append(child);
        }
        for (int k = deferred.size() - 1; k >= 0; --k)
            order.append(deferred.at(k));
    };
    QVector<int> deferredStrips;
    for (int i = 0; i < node.strips.size(); ++i) {
        const Node::Strip &strip = node.strips.at(i);
        const bool last = i == node.strips.size() - 1;
        const bool cameraOnStripSide = strip.vertical ? cameraX < strip.edge
                                                      : cameraY < strip.edge;
        if (last || !cameraOnStripSide)
            emitStrip(strip);
        else
            deferredStrips.append(i);
    }
    for (int i = deferredStrips.size() - 1; i >= 0; --i)
        emitStrip(node.strips.at(deferredStrips.at(i)));
    return order;
}

std::shared_ptr<Code> prepareCode(const QString &text, const QString &language)
{
    auto code = std::make_shared<Code>();
    code->language = language;
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList lines = normalized.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    code->totalLines = lines.size();
    if (lines.size() > kMaxLines)
        lines = lines.mid(0, kMaxLines);

    // Multi-line spans that read as comments: C-style blocks, markup comments,
    // and Python docstrings (one delimiter opens and closes).
    const bool python = language == QLatin1String("Python");
    const bool markup = noSlashCommentLanguages().contains(language) && !python;
    const QString open = python ? QStringLiteral("\"\"\"")
                                : markup ? QStringLiteral("<!--") : QStringLiteral("/*");
    const QString close = python ? open : markup ? QStringLiteral("-->") : QStringLiteral("*/");
    const bool useBlocks = !markup || language == QLatin1String("HTML")
        || language == QLatin1String("XML") || language == QLatin1String("SVG")
        || language == QLatin1String("Markdown");
    code->blockClose = close;
    code->startsInBlock.resize(lines.size());
    QVector<int> lengths;
    lengths.reserve(lines.size());
    bool inside = false;
    for (int i = 0; i < lines.size(); ++i) {
        QString &line = lines[i];
        if (line.contains(QLatin1Char('\t')))
            line.replace(QLatin1Char('\t'), QStringLiteral("    "));
        if (line.size() > kMaxLineChars)
            line.truncate(kMaxLineChars);
        lengths.append(line.size());
        if (!useBlocks)
            continue;
        code->startsInBlock[i] = inside;
        int from = 0;
        for (;;) {
            const QString &marker = inside ? close : open;
            const int at = line.indexOf(marker, from);
            if (at < 0)
                break;
            from = at + marker.size();
            inside = !inside;
        }
    }
    std::sort(lengths.begin(), lengths.end());
    // Column width follows the 97th-percentile line so one minified line does
    // not squash every other line in the file into a sliver.
    const int wide = lengths.isEmpty()
        ? 0
        : lengths.at(std::min<int>(lengths.size() - 1, int(lengths.size() * 0.97)));
    code->maxLength = std::max(12, std::min(110, wide));
    code->lines = lines;
    code->tokens.resize(lines.size());
    code->tokenized.resize(lines.size());
    return code;
}

const QVector<Token> &tokenizeLine(Code &code, int index)
{
    QVector<Token> &out = code.tokens[index];
    if (code.tokenized.at(index))
        return out;
    code.tokenized[index] = true;
    const QString &line = code.lines.at(index);
    const QString &language = code.language;
    int start = 0;
    if (code.startsInBlock.at(index)) {
        const int end = line.indexOf(code.blockClose);
        const int stop = end < 0 ? line.size() : end + code.blockClose.size();
        const int lead = leadingSpaces(line);
        if (stop > lead)
            out.append({lead, stop - lead, 3});
        start = stop;
    }
    const bool prose = proseLanguages().contains(language);
    const bool slashComments = !noSlashCommentLanguages().contains(language);
    const bool hashComments = hashCommentLanguages().contains(language);
    const bool dashComments = dashCommentLanguages().contains(language);
    int pos = start;
    while (pos < line.size()) {
        const QRegularExpressionMatch match = tokenPattern().match(
            line, pos, QRegularExpression::NormalMatch,
            QRegularExpression::AnchorAtOffsetMatchOption);
        if (!match.hasMatch()) {
            ++pos;
            continue;
        }
        const int at = pos;
        const int length = std::max<int>(1, match.capturedLength(0));
        pos = at + length;
        if (match.capturedLength(1) > 0)
            continue;
        int kind = 0;
        bool demote = false;
        if (match.capturedLength(2) > 0) {
            if (slashComments) kind = 3; else demote = true;
        } else if (match.capturedLength(3) > 0) {
            if (hashComments) kind = 3; else demote = true;
        } else if (match.capturedLength(4) > 0) {
            if (dashComments) kind = 3; else demote = true;
        } else if (match.capturedLength(5) > 0) {
            kind = 3;
        } else if (match.capturedLength(6) > 0) {
            if (prose) demote = true;
            else kind = match.capturedView(6).startsWith(QLatin1String("\"\"\"")) ? 3 : 2;
        } else if (match.capturedLength(7) > 0) {
            kind = 4;
        } else if (match.capturedLength(8) > 0) {
            if (!prose && keywords().contains(match.captured(8)))
                kind = 1;
            else if (!prose && at + length < line.size()
                     && line.at(at + length) == QLatin1Char('('))
                kind = 5;
        } else {
            kind = 6;
        }
        if (demote) {
            // The comment/string opener means something else in this language:
            // emit its first character alone and rescan from the next one.
            out.append({at, 1, 6});
            pos = at + 1;
            continue;
        }
        out.append({at, length, kind});
    }
    return out;
}

bool fitCode(Code &code, const Node &node)
{
    const QSizeF size = node.rect.size();
    if (code.fittedFor == size)
        return false;
    code.fittedFor = size;
    const double inset = std::min(size.width(), size.height()) * 0.035;
    const double cw = std::max(0.0, size.width() - inset * 2);
    const double ch = std::max(0.0, size.height() - node.band - inset * 2);
    const int count = std::max<int>(1, code.lines.size());
    const double chars = code.maxLength + 3;
    double bestHeight = 0;
    int bestColumns = 1;
    const int limit = std::min(count, 64);
    for (int columns = 1; columns <= limit; ++columns) {
        const int rows = (count + columns - 1) / columns;
        const double height = std::min(ch / rows, cw / (columns * chars * kCharAspect));
        if (height > bestHeight) {
            bestHeight = height;
            bestColumns = columns;
        }
    }
    code.columns = bestColumns;
    code.rows = (count + bestColumns - 1) / bestColumns;
    code.lineHeight = bestHeight;
    code.columnWidth = chars * kCharAspect * bestHeight;
    code.area = QRectF(inset, node.band + inset, cw, ch);
    return true;
}

const QVector<double> &levels()
{
    // Bitmap line height in device pixels. The fractional levels are the far
    // view: thousands of sub-pixel line bars that antialias into a density
    // texture, so a large file still reads as "full of code" from a distance.
    static const QVector<double> ladder = {0.125, 0.25, 0.5, 1, 2, 4, 8, 16};
    return ladder;
}

double pickLevel(double devicePixelsPerLine)
{
    const double want = devicePixelsPerLine * 0.85;
    for (double level : levels()) {
        if (level >= want)
            return level;
    }
    return want > levels().last() * 1.4 ? 0.0 : levels().last();
}

} // namespace forkmesh::codemap
