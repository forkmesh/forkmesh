#include "RepoInsight.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPromise>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace forkmesh::insight {

namespace {

enum class Syntax { Braces, Python, Keyword, Css };
enum class Comments { CLike, Hash, Lua, Css };

struct LanguageSpec {
    QString name;
    Syntax syntax = Syntax::Braces;
    Comments comments = Comments::CLike;
};

const QHash<QString, LanguageSpec> &languageTable()
{
    static const QHash<QString, LanguageSpec> table = [] {
        QHash<QString, LanguageSpec> t;
        const auto add = [&t](std::initializer_list<const char *> extensions, const char *name,
                              Syntax syntax, Comments comments) {
            for (const char *extension : extensions)
                t.insert(QString::fromLatin1(extension),
                         LanguageSpec{QString::fromLatin1(name), syntax, comments});
        };
        add({"c", "h"}, "C", Syntax::Braces, Comments::CLike);
        add({"cc", "cpp", "cxx", "c++", "hpp", "hh", "hxx", "ipp", "inl", "tpp"}, "C++",
            Syntax::Braces, Comments::CLike);
        add({"m", "mm"}, "Objective-C", Syntax::Braces, Comments::CLike);
        add({"js", "mjs", "cjs", "jsx"}, "JavaScript", Syntax::Braces, Comments::CLike);
        add({"ts", "tsx", "mts", "cts"}, "TypeScript", Syntax::Braces, Comments::CLike);
        add({"vue"}, "Vue", Syntax::Braces, Comments::CLike);
        add({"svelte"}, "Svelte", Syntax::Braces, Comments::CLike);
        add({"py", "pyw", "pyi"}, "Python", Syntax::Python, Comments::Hash);
        add({"go"}, "Go", Syntax::Braces, Comments::CLike);
        add({"rs"}, "Rust", Syntax::Braces, Comments::CLike);
        add({"java"}, "Java", Syntax::Braces, Comments::CLike);
        add({"kt", "kts"}, "Kotlin", Syntax::Braces, Comments::CLike);
        add({"swift"}, "Swift", Syntax::Braces, Comments::CLike);
        add({"cs"}, "C#", Syntax::Braces, Comments::CLike);
        add({"php"}, "PHP", Syntax::Braces, Comments::CLike);
        add({"dart"}, "Dart", Syntax::Braces, Comments::CLike);
        add({"scala", "sc"}, "Scala", Syntax::Braces, Comments::CLike);
        add({"groovy", "gradle"}, "Groovy", Syntax::Braces, Comments::CLike);
        add({"qml"}, "QML", Syntax::Braces, Comments::CLike);
        add({"sh", "bash", "zsh"}, "Shell", Syntax::Braces, Comments::Hash);
        add({"pl", "pm"}, "Perl", Syntax::Braces, Comments::Hash);
        add({"rb", "rake"}, "Ruby", Syntax::Keyword, Comments::Hash);
        add({"ex", "exs"}, "Elixir", Syntax::Keyword, Comments::Hash);
        add({"lua"}, "Lua", Syntax::Keyword, Comments::Lua);
        add({"css"}, "CSS", Syntax::Css, Comments::Css);
        add({"scss", "sass"}, "SCSS", Syntax::Css, Comments::CLike);
        add({"less"}, "Less", Syntax::Css, Comments::CLike);
        return t;
    }();
    return table;
}

QString extensionOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot <= slash + 1)
        return {};
    return path.mid(dot + 1).toLower();
}

LanguageSpec specFor(const QString &path)
{
    return languageTable().value(extensionOf(path));
}

QString dirOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

QString fileNameOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? path : path.mid(slash + 1);
}

QString stemOf(const QString &path)
{
    const QString name = fileNameOf(path);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    return dot <= 0 ? name : name.left(dot);
}

QString withoutExtension(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    return dot <= slash + 1 ? path : path.left(dot);
}

QString joinPath(const QString &dir, const QString &rest)
{
    if (dir.isEmpty())
        return rest;
    if (rest.isEmpty())
        return dir;
    return dir + QLatin1Char('/') + rest;
}

QString normalizedPath(const QString &path)
{
    if (path.isEmpty())
        return {};
    QString clean = QDir::cleanPath(path);
    while (clean.startsWith(QLatin1String("./")))
        clean = clean.mid(2);
    if (clean == QLatin1String(".") || clean.startsWith(QLatin1String("../")) ||
        clean == QLatin1String("..") || clean.startsWith(QLatin1Char('/')))
        return {};
    return clean;
}

int commonPrefix(const QString &a, const QString &b)
{
    const int n = int(qMin(a.size(), b.size()));
    int i = 0;
    while (i < n && a.at(i) == b.at(i))
        ++i;
    return i;
}

struct Stripped {
    QString noComments;
    QString code;
};

Stripped stripSource(const QString &text, const LanguageSpec &spec)
{
    Stripped out;
    out.noComments = text;
    out.code = text;
    QChar *nc = out.noComments.data();
    QChar *cd = out.code.data();
    const qsizetype n = text.size();
    const bool python = spec.syntax == Syntax::Python;
    const bool backticks = spec.name == QLatin1String("JavaScript") ||
                           spec.name == QLatin1String("TypeScript") ||
                           spec.name == QLatin1String("Vue") ||
                           spec.name == QLatin1String("Svelte") || spec.name == QLatin1String("Go") ||
                           spec.name == QLatin1String("Shell");
    const bool rust = spec.name == QLatin1String("Rust");
    const bool cFamily = spec.name == QLatin1String("C") || spec.name == QLatin1String("C++") ||
                         spec.name == QLatin1String("Objective-C");
    const bool shell = spec.name == QLatin1String("Shell") || spec.name == QLatin1String("Perl");
    const auto blank = [](QChar *buffer, qsizetype at) {
        if (buffer[at] != QLatin1Char('\n'))
            buffer[at] = QLatin1Char(' ');
    };
    qsizetype i = 0;
    while (i < n) {
        const QChar c = text.at(i);
        const QChar next = i + 1 < n ? text.at(i + 1) : QChar();
        const bool lineComment =
            (spec.comments == Comments::CLike && c == QLatin1Char('/') && next == QLatin1Char('/')) ||
            (spec.comments == Comments::Lua && c == QLatin1Char('-') && next == QLatin1Char('-')) ||
            (spec.comments == Comments::Hash && c == QLatin1Char('#') &&
             !(shell && i > 0 &&
               (text.at(i - 1) == QLatin1Char('$') || text.at(i - 1) == QLatin1Char('{') ||
                !(text.at(i - 1).isSpace() || text.at(i - 1) == QLatin1Char(';')))));
        if (spec.comments == Comments::Lua && lineComment && i + 3 < n &&
            text.at(i + 2) == QLatin1Char('[') && text.at(i + 3) == QLatin1Char('[')) {
            const qsizetype end = text.indexOf(QLatin1String("]]"), i + 4);
            const qsizetype stop = end < 0 ? n : end + 2;
            for (qsizetype k = i; k < stop; ++k) {
                blank(nc, k);
                blank(cd, k);
            }
            i = stop;
            continue;
        }
        if (lineComment) {
            while (i < n && text.at(i) != QLatin1Char('\n')) {
                blank(nc, i);
                blank(cd, i);
                ++i;
            }
            continue;
        }
        if ((spec.comments == Comments::CLike || spec.comments == Comments::Css) &&
            c == QLatin1Char('/') && next == QLatin1Char('*')) {
            const qsizetype end = text.indexOf(QLatin1String("*/"), i + 2);
            const qsizetype stop = end < 0 ? n : end + 2;
            for (qsizetype k = i; k < stop; ++k) {
                blank(nc, k);
                blank(cd, k);
            }
            i = stop;
            continue;
        }
        const bool quote = c == QLatin1Char('"') || c == QLatin1Char('\'') ||
                           (backticks && c == QLatin1Char('`'));
        if (quote) {
            if (c == QLatin1Char('\'')) {
                if (rust && i + 2 < n && (text.at(i + 1).isLetter() || text.at(i + 1) == QLatin1Char('_')) &&
                    text.at(i + 2) != QLatin1Char('\'')) {
                    ++i;
                    continue;
                }
                if (cFamily && i > 0 && text.at(i - 1).isDigit()) {
                    ++i;
                    continue;
                }
                if (spec.syntax == Syntax::Keyword && spec.name == QLatin1String("Elixir")) {
                    ++i;
                    continue;
                }
            }
            const bool triple = python && i + 2 < n && text.at(i + 1) == c && text.at(i + 2) == c;
            const bool multiline = triple || c == QLatin1Char('`');
            qsizetype k = i + (triple ? 3 : 1);
            while (k < n) {
                const QChar ch = text.at(k);
                if (ch == QLatin1Char('\\')) {
                    k += 2;
                    continue;
                }
                if (!multiline && ch == QLatin1Char('\n'))
                    break;
                if (ch == c) {
                    if (!triple)
                        break;
                    if (k + 2 < n && text.at(k + 1) == c && text.at(k + 2) == c)
                        break;
                }
                ++k;
            }
            const qsizetype contentStart = i + (triple ? 3 : 1);
            const qsizetype contentEnd = qMin(k, n);
            for (qsizetype m = contentStart; m < contentEnd; ++m)
                blank(cd, m);
            i = qMin(n, contentEnd + (k < n && text.at(k) == c ? (triple ? 3 : 1) : 0));
            continue;
        }
        ++i;
    }
    if (cFamily) {
        qsizetype lineStart = 0;
        while (lineStart < n) {
            qsizetype lineEnd = out.code.indexOf(QLatin1Char('\n'), lineStart);
            if (lineEnd < 0)
                lineEnd = n;
            qsizetype first = lineStart;
            while (first < lineEnd && out.code.at(first).isSpace())
                ++first;
            if (first < lineEnd && out.code.at(first) == QLatin1Char('#')) {
                qsizetype end = lineEnd;
                while (true) {
                    qsizetype last = end - 1;
                    while (last > lineStart && out.code.at(last).isSpace())
                        --last;
                    for (qsizetype m = lineStart; m < end; ++m)
                        blank(cd, m);
                    if (last >= lineStart && out.code.at(last) == QLatin1Char('\\') && end < n) {
                        lineStart = end + 1;
                        end = out.code.indexOf(QLatin1Char('\n'), lineStart);
                        if (end < 0)
                            end = n;
                        continue;
                    }
                    break;
                }
                lineEnd = end;
            }
            lineStart = lineEnd + 1;
        }
    }
    return out;
}

struct Patterns {
    QRegularExpression controlStart{QStringLiteral(
        "^(if|else|for|foreach|while|do|switch|try|catch|finally|synchronized|using|lock|unsafe|"
        "checked|unchecked|fixed|with|when|loop|match|select|defer|go|return|case|guard|"
        "repeat|until|unless|elif|then|throw|new|await|yield|get|set|willSet|didSet)\\b")};
    QRegularExpression typeHeader{QStringLiteral(
        "\\b(class|struct|interface|enum|namespace|union|trait|impl|protocol|extension|record|"
        "object|module|package|actor)\\b")};
    QRegularExpression functionKeyword{QStringLiteral("\\b(function|func|fn|fun|def|sub)\\b")};
    QRegularExpression computedProperty{QStringLiteral("\\bvar\\s+([A-Za-z_]\\w*)\\s*:[^=]*$")};
    QRegularExpression callName{QStringLiteral(
        "([A-Za-z_$~][\\w$]*(?:(?:::|\\.)~?[A-Za-z_$][\\w$]*)*|operator\\s*[^\\s(]+)\\s*(?:<[^()]*>)?\\s*\\(")};
    QRegularExpression arrowName{QStringLiteral(
        "([A-Za-z_$][\\w$]*)\\s*[:=]\\s*(?:async\\s*)?(?:\\([^)]*\\)|[A-Za-z_$][\\w$]*)\\s*(?::[^=]*)?=>")};
    QRegularExpression tailAfterParen{QStringLiteral(
        "^[\\s\\w:&*<>,\\[\\].?!\\-|@]*$")};
    QRegularExpression decisionsBase{QStringLiteral(
        "\\b(if|for|foreach|while|case|catch|guard|when|except|elif|elsif|elseif|unless|until|rescue)\\b|&&|\\|\\|")};
    QRegularExpression ternaryStrict{QStringLiteral("\\s\\?\\s")};
    QRegularExpression rustArms{QStringLiteral("=>")};
    QRegularExpression pythonDecisions{QStringLiteral("\\b(if|elif|for|while|except|and|or|case)\\b")};
    QRegularExpression keywordDecisions{QStringLiteral(
        "\\b(if|elsif|elseif|unless|while|until|for|when|rescue|and|or|cond|case)\\b|&&|\\|\\|")};
    QRegularExpression pythonDef{QStringLiteral("^(?:async\\s+)?def\\s+([A-Za-z_]\\w*)\\s*\\(")};
    QRegularExpression pythonClass{QStringLiteral("^class\\s+([A-Za-z_]\\w*)")};
    QRegularExpression pythonBlock{QStringLiteral(
        "^(if|elif|else|for|while|try|except|finally|with|match|case|async\\s+for|async\\s+with)\\b.*:$")};
    QRegularExpression keywordDef{QStringLiteral(
        "^\\s*(?:local\\s+)?(?:def|defp|defmacro|function)\\s+([A-Za-z_][\\w.:?!]*)")};
    QRegularExpression keywordOpen{QStringLiteral(
        "\\b(if|unless|while|until|for|case|begin|do|function|def|defp|class|module)\\b")};
    QRegularExpression keywordEnd{QStringLiteral("\\bend\\b")};
    QRegularExpression typeDeclaration{QStringLiteral(
        "\\b(class|struct|interface|enum|protocol|record|object|actor|trait|typealias)\\s+([A-Z][A-Za-z0-9_]*)")};
    QRegularExpression packageDeclaration{QStringLiteral(
        "^\\s*(?:package|namespace)\\s+([A-Za-z_][\\w.]*)")};
    QRegularExpression capitalized{QStringLiteral("\\b[A-Z][A-Za-z0-9_]{2,}\\b")};
    QRegularExpression pureVirtual{QStringLiteral("\\)\\s*(?:const\\s*)?=\\s*0\\s*;")};
    QRegularExpression cInclude{QStringLiteral("^\\s*#\\s*(?:include|import)\\s*([\"<])([^\">\\n]+)[\">]")};
    QRegularExpression jsImport{QStringLiteral(
        "\\bimport\\s+(?:type\\s+)?(?:[\\w$*{}\\s,]+?\\s+from\\s+)?[\"']([^\"'\\n]+)[\"']")};
    QRegularExpression jsExport{QStringLiteral(
        "\\bexport\\s+(?:type\\s+)?(?:\\*|\\{[^}]*\\}|\\*\\s+as\\s+\\w+)\\s+from\\s+[\"']([^\"'\\n]+)[\"']")};
    QRegularExpression jsRequire{QStringLiteral(
        "\\b(?:require|import)\\s*\\(\\s*[\"']([^\"'\\n]+)[\"']\\s*\\)")};
    QRegularExpression pyImport{QStringLiteral(
        "^[ \\t]*import[ \\t]+([\\w.]+(?:[ \\t]+as[ \\t]+\\w+)?(?:[ \\t]*,[ \\t]*[\\w.]+(?:[ \\t]+as[ \\t]+\\w+)?)*)")};
    QRegularExpression pyFrom{QStringLiteral(
        "^[ \\t]*from[ \\t]+(\\.*[\\w.]*)[ \\t]+import[ \\t]+(\\([^)]*\\)|[^\\n]+)")};
    QRegularExpression goBlock{QStringLiteral("\\bimport\\s*\\(([^)]*)\\)")};
    QRegularExpression goSingle{QStringLiteral("\\bimport\\s+(?:[\\w.]+\\s+)?\"([^\"\\n]+)\"")};
    QRegularExpression quoted{QStringLiteral("\"([^\"\\n]+)\"")};
    QRegularExpression rustMod{QStringLiteral(
        "^\\s*(?:pub(?:\\([^)]*\\))?\\s+)?mod\\s+([A-Za-z_]\\w*)\\s*;")};
    QRegularExpression rustUse{QStringLiteral("\\buse\\s+((?:crate|super|self)(?:::[A-Za-z_]\\w*)+)")};
    QRegularExpression jvmImport{QStringLiteral(
        "^\\s*import\\s+(?:static\\s+)?([A-Za-z_][\\w.]*)(\\.\\*)?")};
    QRegularExpression csUsing{QStringLiteral(
        "^\\s*(?:global\\s+)?using\\s+(?:static\\s+)?([A-Za-z_][\\w.]*)\\s*;")};
    QRegularExpression swiftImport{QStringLiteral("^\\s*(?:@\\w+\\s+)*import\\s+(?:\\w+\\s+)?([A-Za-z_][\\w.]*)")};
    QRegularExpression rubyRequire{QStringLiteral(
        "\\brequire(_relative)?\\s*\\(?\\s*[\"']([^\"'\\n]+)[\"']")};
    QRegularExpression phpInclude{QStringLiteral(
        "\\b(?:require|include)(?:_once)?\\s*\\(?\\s*(?:__DIR__\\s*\\.\\s*)?[\"']([^\"'\\n]+)[\"']")};
    QRegularExpression phpUse{QStringLiteral("^\\s*use\\s+([A-Za-z_][\\w\\\\]*)")};
    QRegularExpression dartImport{QStringLiteral(
        "^\\s*(?:import|export|part)\\s+[\"']([^\"'\\n]+)[\"']")};
    QRegularExpression luaRequire{QStringLiteral("\\brequire\\s*\\(?\\s*[\"']([^\"'\\n]+)[\"']")};
    QRegularExpression cssImport{QStringLiteral(
        "@(?:import|use|forward)\\s+(?:url\\()?\\s*[\"']([^\"'\\n]+)[\"']")};
    QRegularExpression qmlImport{QStringLiteral("^\\s*import\\s+([A-Za-z_][\\w.]*|\"[^\"]+\")")};
    QRegularExpression shellSource{QStringLiteral(
        "(?:^|[;&|]\\s*|\\s)(?:source|\\.)\\s+[\"']?([\\w./${}-]+\\.(?:sh|bash))")};
    QRegularExpression elixirAlias{QStringLiteral("^\\s*(?:alias|import|use|require)\\s+([A-Z][\\w.]*)")};
    QRegularExpression whitespaceRun{QStringLiteral("\\s+")};
};

Patterns &patterns()
{
    thread_local Patterns instance;
    return instance;
}

bool isTypeKeywordCounted(const QString &keyword)
{
    static const QSet<QString> counted = {
        QStringLiteral("class"),  QStringLiteral("struct"),   QStringLiteral("interface"),
        QStringLiteral("enum"),   QStringLiteral("union"),    QStringLiteral("trait"),
        QStringLiteral("protocol"), QStringLiteral("record"), QStringLiteral("object"),
        QStringLiteral("actor")};
    return counted.contains(keyword);
}

bool isAbstractKeyword(const QString &keyword)
{
    return keyword == QLatin1String("interface") || keyword == QLatin1String("protocol") ||
           keyword == QLatin1String("trait");
}

const QSet<QString> &nonNameWords()
{
    static const QSet<QString> words = {
        QStringLiteral("if"),       QStringLiteral("for"),      QStringLiteral("while"),
        QStringLiteral("switch"),   QStringLiteral("catch"),    QStringLiteral("return"),
        QStringLiteral("new"),      QStringLiteral("throw"),    QStringLiteral("await"),
        QStringLiteral("sizeof"),   QStringLiteral("typeof"),   QStringLiteral("delete"),
        QStringLiteral("case"),     QStringLiteral("function"), QStringLiteral("func"),
        QStringLiteral("fn"),       QStringLiteral("fun"),      QStringLiteral("def"),
        QStringLiteral("async"),    QStringLiteral("decltype"), QStringLiteral("alignof"),
        QStringLiteral("static_assert"), QStringLiteral("foreach"), QStringLiteral("using"),
        QStringLiteral("lock"),     QStringLiteral("when"),     QStringLiteral("match"),
        QStringLiteral("elif"),     QStringLiteral("assert"),   QStringLiteral("super"),
        QStringLiteral("this"),     QStringLiteral("sub")};
    return words;
}

QString lastSegment(const QString &qualified)
{
    int cut = int(qMax(qualified.lastIndexOf(QLatin1String("::")), qualified.lastIndexOf(QLatin1Char('.'))));
    if (cut < 0)
        return qualified;
    return qualified.mid(cut + (qualified.at(cut) == QLatin1Char(':') ? 2 : 1));
}

int matchingParen(const QString &text, qsizetype open)
{
    int depth = 0;
    for (qsizetype i = open; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('('))
            ++depth;
        else if (c == QLatin1Char(')')) {
            --depth;
            if (depth == 0)
                return int(i);
        }
    }
    return -1;
}

int countParameters(const QString &inside, bool python)
{
    const QString trimmed = inside.trimmed();
    if (trimmed.isEmpty() || trimmed == QLatin1String("void"))
        return 0;
    int depth = 0;
    QStringList parts;
    QString current;
    for (const QChar c : trimmed) {
        if (c == QLatin1Char('(') || c == QLatin1Char('<') || c == QLatin1Char('[') ||
            c == QLatin1Char('{'))
            ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char('>') || c == QLatin1Char(']') ||
                 c == QLatin1Char('}'))
            depth = qMax(0, depth - 1);
        if (c == QLatin1Char(',') && depth == 0) {
            parts.append(current.trimmed());
            current.clear();
            continue;
        }
        current.append(c);
    }
    parts.append(current.trimmed());
    int count = 0;
    for (int i = 0; i < parts.size(); ++i) {
        const QString part = parts.at(i);
        if (part.isEmpty())
            continue;
        if (python && i == 0 &&
            (part == QLatin1String("self") || part == QLatin1String("cls")))
            continue;
        if (python && (part == QLatin1String("*") || part == QLatin1String("/")))
            continue;
        ++count;
    }
    return count;
}

enum class HeaderKind { Block, Function, Type, Container };

struct HeaderInfo {
    HeaderKind kind = HeaderKind::Block;
    QString name;
    int parameters = 0;
    bool abstract = false;
    bool countedType = false;
};

QString cleanHeader(const QString &raw)
{
    const QStringList lines = raw.split(QLatin1Char('\n'));
    QStringList kept;
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (t.isEmpty())
            continue;
        if (t.startsWith(QLatin1Char('#')) || (t.startsWith(QLatin1Char('@')) && !t.contains(QLatin1Char(' ')) &&
                                               !t.contains(QLatin1Char('('))))
            continue;
        if (t.startsWith(QLatin1Char('[')) && t.endsWith(QLatin1Char(']')))
            continue;
        kept.append(t);
    }
    if (kept.size() > 1) {
        static const char *continuationEnds[] = {"(", ",", "=", ":", "->", "=>", "<", "&", "|", "+",
                                                 ".", "["};
        static const char *continuationStarts[] = {",", ":", ".", "->", ")", "where", "throws",
                                                   "const", "override", "noexcept", "final", "?",
                                                   "&&", "||", "async", "->"};
        int first = int(kept.size()) - 1;
        int balance = 0;
        for (const QChar c : kept.at(first))
            balance += c == QLatin1Char('(') ? 1 : c == QLatin1Char(')') ? -1 : 0;
        while (first > 0) {
            const QString &above = kept.at(first - 1);
            const QString &current = kept.at(first);
            bool join = balance < 0;
            for (const char *end : continuationEnds)
                join = join || above.endsWith(QLatin1String(end));
            for (const char *start : continuationStarts)
                join = join || current.startsWith(QLatin1String(start));
            if (!join)
                break;
            --first;
            for (const QChar c : kept.at(first))
                balance += c == QLatin1Char('(') ? 1 : c == QLatin1Char(')') ? -1 : 0;
        }
        kept = kept.mid(first);
    }
    QString header = kept.join(QLatin1Char(' '));
    while (header.startsWith(QLatin1Char('@'))) {
        int space = int(header.indexOf(QLatin1Char(' ')));
        int paren = int(header.indexOf(QLatin1Char('(')));
        if (paren >= 0 && (space < 0 || paren < space)) {
            const int close = matchingParen(header, paren);
            if (close < 0)
                break;
            header = header.mid(close + 1).trimmed();
        } else if (space > 0) {
            header = header.mid(space + 1).trimmed();
        } else {
            break;
        }
    }
    return header;
}

HeaderInfo classifyHeader(const QString &rawHeader, const QString &language)
{
    HeaderInfo info;
    const QString header = cleanHeader(rawHeader);
    if (header.isEmpty())
        return info;
    Patterns &p = patterns();
    if (p.controlStart.match(header).hasMatch())
        return info;
    const qsizetype firstParen = header.indexOf(QLatin1Char('('));
    const QString beforeParen = firstParen < 0 ? header : header.left(firstParen);
    const bool keywordFunction = p.functionKeyword.match(header).hasMatch();
    const QRegularExpressionMatch type = p.typeHeader.match(beforeParen);
    if (type.hasMatch() && !keywordFunction) {
        const QString keyword = type.captured(1);
        info.kind = isTypeKeywordCounted(keyword) ? HeaderKind::Type : HeaderKind::Container;
        info.countedType = isTypeKeywordCounted(keyword);
        info.abstract = isAbstractKeyword(keyword) ||
                        header.contains(QLatin1String("abstract "));
        const QString after = beforeParen.mid(type.capturedEnd(1)).trimmed();
        info.name = after.section(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), 0, 0);
        return info;
    }
    bool function = keywordFunction;
    if (!function && header.endsWith(QLatin1String("=>")))
        function = true;
    if (!function && (language == QLatin1String("Swift") || language == QLatin1String("Kotlin"))) {
        const QRegularExpressionMatch computed = p.computedProperty.match(header);
        if (computed.hasMatch()) {
            info.kind = HeaderKind::Function;
            info.name = computed.captured(1);
            return info;
        }
    }
    if (!function && firstParen > 0) {
        const qsizetype lastClose = header.lastIndexOf(QLatin1Char(')'));
        if (lastClose > firstParen) {
            const QString tail = header.mid(lastClose + 1);
            int depth = 0;
            bool assignment = false;
            for (qsizetype k = 0; k < firstParen; ++k) {
                const QChar c = header.at(k);
                if (c == QLatin1Char('<') || c == QLatin1Char('['))
                    ++depth;
                else if (c == QLatin1Char('>') || c == QLatin1Char(']'))
                    depth = qMax(0, depth - 1);
                else if (c == QLatin1Char('=') && depth == 0) {
                    const QChar prev = k > 0 ? header.at(k - 1) : QChar();
                    const QChar nxt = k + 1 < header.size() ? header.at(k + 1) : QChar();
                    if (nxt != QLatin1Char('=') && prev != QLatin1Char('=') && prev != QLatin1Char('!') &&
                        prev != QLatin1Char('<') && prev != QLatin1Char('>') &&
                        !header.left(k).contains(QLatin1String("operator")))
                        assignment = true;
                }
            }
            const bool tailOk = p.tailAfterParen.match(tail).hasMatch() ||
                                tail.trimmed().startsWith(QLatin1Char(':'));
            if (!assignment && tailOk) {
                const QRegularExpressionMatch call = p.callName.match(header.left(firstParen + 1));
                if (call.hasMatch() && !nonNameWords().contains(lastSegment(call.captured(1))))
                    function = true;
            }
        }
    }
    if (!function)
        return info;
    info.kind = HeaderKind::Function;
    QRegularExpressionMatchIterator it = p.callName.globalMatch(header);
    qsizetype paramsOpen = -1;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString candidate = m.captured(1);
        if (nonNameWords().contains(lastSegment(candidate)) ||
            nonNameWords().contains(candidate))
            continue;
        info.name = candidate;
        paramsOpen = m.capturedEnd(0) - 1;
        break;
    }
    if (info.name.isEmpty()) {
        const QRegularExpressionMatch arrow = p.arrowName.match(header);
        if (arrow.hasMatch())
            info.name = arrow.captured(1);
    }
    if (paramsOpen < 0)
        paramsOpen = header.indexOf(QLatin1Char('('));
    if (paramsOpen >= 0) {
        const int close = matchingParen(header, paramsOpen);
        if (close > paramsOpen)
            info.parameters = countParameters(header.mid(paramsOpen + 1, close - paramsOpen - 1),
                                              false);
    }
    if (info.name.isEmpty())
        info.name = QStringLiteral("(anonymous)");
    static const QSet<QString> scriptLike = {QStringLiteral("JavaScript"), QStringLiteral("TypeScript"),
                                             QStringLiteral("Vue"), QStringLiteral("Svelte")};
    static const QSet<QString> wrappers = {
        QStringLiteral("(anonymous)"), QStringLiteral("define"), QStringLiteral("describe"),
        QStringLiteral("suite"), QStringLiteral("context"), QStringLiteral("$"),
        QStringLiteral("jQuery"), QStringLiteral("ready"), QStringLiteral("addEventListener")};
    if (scriptLike.contains(language) && wrappers.contains(lastSegment(info.name)))
        info.kind = HeaderKind::Container;
    return info;
}

int countMatches(const QRegularExpression &re, QStringView text)
{
    int count = 0;
    QRegularExpressionMatchIterator it = re.globalMatchView(text);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

QVector<qsizetype> decisionPositions(const QString &code, const QString &language)
{
    Patterns &p = patterns();
    QVector<qsizetype> positions;
    QRegularExpressionMatchIterator it = p.decisionsBase.globalMatch(code);
    while (it.hasNext())
        positions.append(it.next().capturedStart(0));
    static const QSet<QString> strictTernary = {
        QStringLiteral("TypeScript"), QStringLiteral("Swift"), QStringLiteral("Kotlin"),
        QStringLiteral("C#"), QStringLiteral("Dart"), QStringLiteral("Vue"),
        QStringLiteral("Svelte"), QStringLiteral("Scala")};
    static const QSet<QString> looseTernary = {
        QStringLiteral("C"), QStringLiteral("C++"), QStringLiteral("Objective-C"),
        QStringLiteral("Java"), QStringLiteral("JavaScript"), QStringLiteral("PHP"),
        QStringLiteral("Groovy"), QStringLiteral("Perl"), QStringLiteral("QML")};
    if (strictTernary.contains(language)) {
        QRegularExpressionMatchIterator t = p.ternaryStrict.globalMatch(code);
        while (t.hasNext())
            positions.append(t.next().capturedStart(0));
    } else if (looseTernary.contains(language)) {
        for (qsizetype i = 0; i < code.size(); ++i) {
            if (code.at(i) != QLatin1Char('?'))
                continue;
            const QChar next = i + 1 < code.size() ? code.at(i + 1) : QChar();
            const QChar prev = i > 0 ? code.at(i - 1) : QChar();
            if (next == QLatin1Char('?') || next == QLatin1Char('.') || next == QLatin1Char(':') ||
                prev == QLatin1Char('?') || next == QLatin1Char('>') || next == QLatin1Char(')') ||
                next == QLatin1Char(','))
                continue;
            positions.append(i);
        }
    } else if (language == QLatin1String("Rust")) {
        QRegularExpressionMatchIterator t = p.rustArms.globalMatch(code);
        while (t.hasNext())
            positions.append(t.next().capturedStart(0));
    }
    std::sort(positions.begin(), positions.end());
    return positions;
}

struct LineStarts {
    QVector<qsizetype> starts;
    explicit LineStarts(const QString &text)
    {
        starts.append(0);
        for (qsizetype i = 0; i < text.size(); ++i)
            if (text.at(i) == QLatin1Char('\n'))
                starts.append(i + 1);
    }
    int lineAt(qsizetype position) const
    {
        const auto it = std::upper_bound(starts.begin(), starts.end(), position);
        return int(it - starts.begin());
    }
};

struct FunctionRange {
    qsizetype bodyStart = 0;
    qsizetype bodyEnd = 0;
};

void analyzeBraces(FileInfo &info, const Stripped &stripped)
{
    const QString &code = stripped.code;
    const LineStarts lines(code);
    struct Frame {
        HeaderKind kind;
        int function;
    };
    QVector<Frame> stack;
    QVector<FunctionRange> ranges;
    qsizetype headerStart = 0;
    int functionDepth = -1;
    int currentFunction = -1;
    int parenDepth = 0;
    for (qsizetype i = 0; i < code.size(); ++i) {
        const QChar c = code.at(i);
        if (c == QLatin1Char('('))
            ++parenDepth;
        else if (c == QLatin1Char(')'))
            parenDepth = qMax(0, parenDepth - 1);
        else if (c == QLatin1Char(';') && parenDepth == 0)
            headerStart = i + 1;
        else if (c == QLatin1Char('{')) {
            if (currentFunction >= 0) {
                stack.append({HeaderKind::Block, currentFunction});
                const int nesting = int(stack.size()) - functionDepth - 1;
                FunctionInfo &fn = info.functions[currentFunction];
                fn.nesting = qMax(fn.nesting, nesting);
            } else {
                const QString header = code.mid(headerStart, i - headerStart);
                const HeaderInfo kind = classifyHeader(header, info.language);
                if (kind.kind == HeaderKind::Function) {
                    FunctionInfo fn;
                    fn.name = kind.name;
                    qsizetype first = headerStart;
                    while (first < i && code.at(first).isSpace())
                        ++first;
                    const QString cleaned = cleanHeader(header);
                    if (!cleaned.isEmpty()) {
                        const qsizetype at = code.indexOf(cleaned.left(qMin<qsizetype>(cleaned.size(), 12)), first);
                        if (at >= 0 && at < i)
                            first = at;
                    }
                    fn.line = lines.lineAt(first);
                    fn.parameters = kind.parameters;
                    info.functions.append(fn);
                    ranges.append({i + 1, i + 1});
                    currentFunction = int(info.functions.size()) - 1;
                    stack.append({HeaderKind::Function, currentFunction});
                    functionDepth = int(stack.size()) - 1;
                } else {
                    if (kind.kind == HeaderKind::Type) {
                        if (kind.countedType)
                            ++info.types;
                        if (kind.abstract)
                            ++info.abstractTypes;
                    }
                    stack.append({kind.kind, -1});
                }
            }
            headerStart = i + 1;
            parenDepth = 0;
        } else if (c == QLatin1Char('}')) {
            if (!stack.isEmpty()) {
                const Frame frame = stack.takeLast();
                if (frame.kind == HeaderKind::Function && int(stack.size()) == functionDepth) {
                    FunctionInfo &fn = info.functions[frame.function];
                    ranges[frame.function].bodyEnd = i;
                    fn.length = qMax(1, lines.lineAt(i) - fn.line + 1);
                    currentFunction = -1;
                    functionDepth = -1;
                }
            }
            headerStart = i + 1;
            parenDepth = 0;
        }
    }
    if (currentFunction >= 0) {
        FunctionInfo &fn = info.functions[currentFunction];
        ranges[currentFunction].bodyEnd = code.size();
        fn.length = qMax(1, lines.lineAt(code.size() - 1) - fn.line + 1);
    }
    const QVector<qsizetype> decisions = decisionPositions(code, info.language);
    int fileLevel = 0;
    for (const qsizetype position : decisions) {
        int lo = 0;
        int hi = int(ranges.size()) - 1;
        int found = -1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (ranges.at(mid).bodyStart <= position) {
                found = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        if (found >= 0 && position < ranges.at(found).bodyEnd)
            ++info.functions[found].complexity;
        else
            ++fileLevel;
    }
    int total = fileLevel;
    for (const FunctionInfo &fn : std::as_const(info.functions)) {
        total += fn.complexity;
        info.maxComplexity = qMax(info.maxComplexity, fn.complexity);
        info.maxNesting = qMax(info.maxNesting, fn.nesting);
    }
    info.complexity = total;
    if (info.types > 0 && info.abstractTypes == 0 &&
        (info.language == QLatin1String("C++") || info.language == QLatin1String("C")) &&
        patterns().pureVirtual.match(code).hasMatch())
        info.abstractTypes = 1;
}

int indentOf(QStringView line)
{
    int indent = 0;
    for (const QChar c : line) {
        if (c == QLatin1Char(' '))
            ++indent;
        else if (c == QLatin1Char('\t'))
            indent += 4;
        else
            break;
    }
    return indent;
}

void analyzePython(FileInfo &info, const Stripped &stripped)
{
    Patterns &p = patterns();
    const QStringList codeLines = stripped.code.split(QLatin1Char('\n'));
    const QStringList textLines = stripped.noComments.split(QLatin1Char('\n'));
    int current = -1;
    int fnIndent = 0;
    int lastCodeLine = 0;
    QVector<int> blocks;
    int fileLevel = 0;
    const auto close = [&](int endLine) {
        if (current < 0)
            return;
        FunctionInfo &fn = info.functions[current];
        fn.length = qMax(1, endLine - fn.line + 1);
        current = -1;
        blocks.clear();
    };
    int continuation = 0;
    for (int i = 0; i < codeLines.size(); ++i) {
        const QString &line = codeLines.at(i);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        const bool continued = continuation > 0;
        for (const QChar c : trimmed) {
            if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{'))
                ++continuation;
            else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}'))
                continuation = qMax(0, continuation - 1);
        }
        const int lineNo = i + 1;
        const int indent = indentOf(line);
        if (!continued && current >= 0 && indent <= fnIndent)
            close(lastCodeLine);
        if (!continued) {
            const QRegularExpressionMatch def = p.pythonDef.match(trimmed);
            if (def.hasMatch() && current < 0) {
                FunctionInfo fn;
                fn.name = def.captured(1);
                fn.line = lineNo;
                const QString source = i < textLines.size() ? textLines.mid(i, 30).join(QLatin1Char('\n'))
                                                            : QString();
                const qsizetype open = source.indexOf(QLatin1Char('('));
                if (open >= 0) {
                    const int closeAt = matchingParen(source, open);
                    if (closeAt > open)
                        fn.parameters = countParameters(source.mid(open + 1, closeAt - open - 1), true);
                }
                info.functions.append(fn);
                current = int(info.functions.size()) - 1;
                fnIndent = indent;
                lastCodeLine = lineNo;
                continue;
            }
            const QRegularExpressionMatch cls = p.pythonClass.match(trimmed);
            if (cls.hasMatch()) {
                ++info.types;
                if (trimmed.contains(QLatin1String("ABC")) || trimmed.contains(QLatin1String("Protocol")) ||
                    trimmed.contains(QLatin1String("ABCMeta")))
                    ++info.abstractTypes;
            }
        }
        const int decisions = countMatches(p.pythonDecisions, trimmed);
        if (current >= 0) {
            FunctionInfo &fn = info.functions[current];
            fn.complexity += decisions;
            if (!continued) {
                while (!blocks.isEmpty() && blocks.last() >= indent)
                    blocks.removeLast();
                fn.nesting = qMax(fn.nesting, int(blocks.size()));
                if (p.pythonBlock.match(trimmed).hasMatch())
                    blocks.append(indent);
            }
        } else {
            fileLevel += decisions;
        }
        lastCodeLine = lineNo;
    }
    close(lastCodeLine);
    int total = fileLevel;
    for (const FunctionInfo &fn : std::as_const(info.functions)) {
        total += fn.complexity;
        info.maxComplexity = qMax(info.maxComplexity, fn.complexity);
        info.maxNesting = qMax(info.maxNesting, fn.nesting);
    }
    info.complexity = total;
}

void analyzeKeyword(FileInfo &info, const Stripped &stripped)
{
    Patterns &p = patterns();
    const QStringList codeLines = stripped.code.split(QLatin1Char('\n'));
    int current = -1;
    int baseDepth = 0;
    int depth = 0;
    int fileLevel = 0;
    int lastCodeLine = 0;
    for (int i = 0; i < codeLines.size(); ++i) {
        const QString trimmed = codeLines.at(i).trimmed();
        if (trimmed.isEmpty())
            continue;
        const QRegularExpressionMatch def = p.keywordDef.match(trimmed);
        if (def.hasMatch()) {
            if (current >= 0)
                info.functions[current].length = qMax(1, lastCodeLine - info.functions[current].line + 1);
            FunctionInfo fn;
            fn.name = def.captured(1);
            fn.line = i + 1;
            const qsizetype open = trimmed.indexOf(QLatin1Char('('));
            if (open >= 0) {
                const int closeAt = matchingParen(trimmed, open);
                if (closeAt > open)
                    fn.parameters = countParameters(trimmed.mid(open + 1, closeAt - open - 1), false);
            }
            info.functions.append(fn);
            current = int(info.functions.size()) - 1;
            baseDepth = depth;
        }
        if (trimmed.startsWith(QLatin1String("class ")) || trimmed.startsWith(QLatin1String("module ")) ||
            trimmed.startsWith(QLatin1String("defmodule ")))
            ++info.types;
        const int opens = countMatches(p.keywordOpen, trimmed) -
                          (trimmed.contains(QLatin1String(" if ")) && !trimmed.startsWith(QLatin1String("if")) ? 1 : 0);
        const int ends = countMatches(p.keywordEnd, trimmed);
        depth = qMax(0, depth + opens - ends);
        const int decisions = countMatches(p.keywordDecisions, trimmed);
        if (current >= 0) {
            FunctionInfo &fn = info.functions[current];
            fn.complexity += decisions;
            fn.nesting = qMax(fn.nesting, qMax(0, depth - baseDepth - 1));
        } else {
            fileLevel += decisions;
        }
        lastCodeLine = i + 1;
    }
    if (current >= 0)
        info.functions[current].length = qMax(1, lastCodeLine - info.functions[current].line + 1);
    int total = fileLevel;
    for (const FunctionInfo &fn : std::as_const(info.functions)) {
        total += fn.complexity;
        info.maxComplexity = qMax(info.maxComplexity, fn.complexity);
        info.maxNesting = qMax(info.maxNesting, fn.nesting);
    }
    info.complexity = total;
}

QStringList importsFromStripped(const QString &language, const QString &text)
{
    Patterns &p = patterns();
    QStringList specs;
    const auto collect = [&specs](const QRegularExpression &re, const QString &source, int group) {
        QRegularExpressionMatchIterator it = re.globalMatch(source);
        while (it.hasNext()) {
            const QString value = it.next().captured(group).trimmed();
            if (!value.isEmpty())
                specs.append(value);
        }
    };
    const auto multiline = [](const QRegularExpression &re) {
        QRegularExpression copy = re;
        copy.setPatternOptions(copy.patternOptions() | QRegularExpression::MultilineOption);
        return copy;
    };
    if (language == QLatin1String("C") || language == QLatin1String("C++") ||
        language == QLatin1String("Objective-C")) {
        QRegularExpressionMatchIterator it = multiline(p.cInclude).globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString path = m.captured(2).trimmed();
            specs.append(m.captured(1) == QLatin1String("<") ? QLatin1Char('<') + path + QLatin1Char('>')
                                                             : path);
        }
        if (language == QLatin1String("Objective-C")) {
            QRegularExpression module(QStringLiteral("^\\s*@import\\s+([\\w.]+)"),
                                      QRegularExpression::MultilineOption);
            QRegularExpressionMatchIterator mit = module.globalMatch(text);
            while (mit.hasNext())
                specs.append(QLatin1Char('<') + mit.next().captured(1) + QLatin1Char('>'));
        }
    } else if (language == QLatin1String("JavaScript") || language == QLatin1String("TypeScript") ||
               language == QLatin1String("Vue") || language == QLatin1String("Svelte")) {
        collect(p.jsImport, text, 1);
        collect(p.jsExport, text, 1);
        collect(p.jsRequire, text, 1);
    } else if (language == QLatin1String("Python")) {
        QRegularExpressionMatchIterator it = multiline(p.pyImport).globalMatch(text);
        while (it.hasNext()) {
            const QStringList parts = it.next().captured(1).split(QLatin1Char(','));
            for (const QString &part : parts) {
                const QString module = part.trimmed().section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
                if (!module.isEmpty())
                    specs.append(module);
            }
        }
        QRegularExpressionMatchIterator from = multiline(p.pyFrom).globalMatch(text);
        while (from.hasNext()) {
            const QRegularExpressionMatch m = from.next();
            const QString module = m.captured(1);
            specs.append(module);
            QString names = m.captured(2);
            names.remove(QLatin1Char('(')).remove(QLatin1Char(')')).remove(QLatin1Char('\\'));
            for (const QString &raw : names.split(QLatin1Char(','))) {
                const QString name = raw.trimmed().section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
                if (name.isEmpty() || name == QLatin1String("*"))
                    continue;
                const QString joined = module.endsWith(QLatin1Char('.')) ? module + name
                                                                         : module + QLatin1Char('.') + name;
                specs.append(QLatin1Char('?') + joined);
            }
        }
    } else if (language == QLatin1String("Go")) {
        QRegularExpressionMatchIterator it = p.goBlock.globalMatch(text);
        while (it.hasNext())
            collect(p.quoted, it.next().captured(1), 1);
        collect(p.goSingle, text, 1);
    } else if (language == QLatin1String("Rust")) {
        QRegularExpressionMatchIterator mods = multiline(p.rustMod).globalMatch(text);
        while (mods.hasNext())
            specs.append(QStringLiteral("mod ") + mods.next().captured(1));
        collect(p.rustUse, text, 1);
    } else if (language == QLatin1String("Java") || language == QLatin1String("Kotlin") ||
               language == QLatin1String("Scala") || language == QLatin1String("Groovy")) {
        QRegularExpressionMatchIterator it = multiline(p.jvmImport).globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            specs.append(m.captured(1) + (m.captured(2).isEmpty() ? QString() : QStringLiteral(".*")));
        }
    } else if (language == QLatin1String("C#")) {
        collect(multiline(p.csUsing), text, 1);
    } else if (language == QLatin1String("Swift")) {
        collect(multiline(p.swiftImport), text, 1);
    } else if (language == QLatin1String("Ruby")) {
        QRegularExpressionMatchIterator it = p.rubyRequire.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            specs.append(m.captured(1).isEmpty() ? m.captured(2)
                                                 : QStringLiteral("./") + m.captured(2));
        }
    } else if (language == QLatin1String("Elixir")) {
        collect(multiline(p.elixirAlias), text, 1);
    } else if (language == QLatin1String("PHP")) {
        collect(p.phpInclude, text, 1);
        collect(multiline(p.phpUse), text, 1);
    } else if (language == QLatin1String("Dart")) {
        collect(multiline(p.dartImport), text, 1);
    } else if (language == QLatin1String("Lua")) {
        collect(p.luaRequire, text, 1);
    } else if (language == QLatin1String("CSS") || language == QLatin1String("SCSS") ||
               language == QLatin1String("Less")) {
        collect(p.cssImport, text, 1);
    } else if (language == QLatin1String("QML")) {
        QRegularExpressionMatchIterator it = multiline(p.qmlImport).globalMatch(text);
        while (it.hasNext()) {
            QString value = it.next().captured(1);
            if (value.startsWith(QLatin1Char('"')))
                value = QStringLiteral("./") + value.mid(1, value.size() - 2);
            specs.append(value);
        }
    } else if (language == QLatin1String("Shell")) {
        collect(multiline(p.shellSource), text, 1);
    }
    QStringList unique;
    QSet<QString> seen;
    for (const QString &spec : std::as_const(specs)) {
        if (seen.contains(spec))
            continue;
        seen.insert(spec);
        unique.append(spec);
    }
    return unique;
}

struct Measured {
    FileInfo info;
    QVector<int> significant;
    QVector<quint64> windows;
    bool skip = false;
};

constexpr int kWindow = 6;

bool boilerplateLine(QStringView line)
{
    static const char *prefixes[] = {"import ", "#include", "#import", "using ", "from ",
                                     "package ", "require", "use ", "@import", "export {",
                                     "export *", "} from", "#pragma", "#define", "#endif",
                                     "#if", "#else", "namespace ", "module "};
    for (const char *prefix : prefixes)
        if (line.startsWith(QLatin1String(prefix)))
            return true;
    return false;
}

void computeWindows(Measured &measured, const QString &noComments)
{
    const QStringList lines = noComments.split(QLatin1Char('\n'));
    QVector<quint64> hashes;
    for (int i = 0; i < lines.size(); ++i) {
        QString normalized = lines.at(i).simplified();
        if (normalized.size() < 4 || boilerplateLine(normalized))
            continue;
        bool punctuation = true;
        for (const QChar c : normalized) {
            if (c.isLetterOrNumber()) {
                punctuation = false;
                break;
            }
        }
        if (punctuation)
            continue;
        measured.significant.append(i + 1);
        hashes.append(quint64(qHash(normalized, 0x51ed270b)) * 0x9e3779b97f4a7c15ULL);
    }
    for (int k = 0; k + kWindow <= hashes.size(); ++k) {
        quint64 h = 0xcbf29ce484222325ULL;
        for (int j = 0; j < kWindow; ++j) {
            h ^= hashes.at(k + j) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h *= 0x100000001b3ULL;
        }
        measured.windows.append(h);
    }
}

void collectSymbols(FileInfo &info, const Stripped &stripped)
{
    static const QSet<QString> typed = {QStringLiteral("Swift"), QStringLiteral("C#"),
                                        QStringLiteral("Kotlin"), QStringLiteral("Java"),
                                        QStringLiteral("Scala"), QStringLiteral("Dart")};
    if (!typed.contains(info.language))
        return;
    Patterns &p = patterns();
    QSet<QString> declared;
    QRegularExpressionMatchIterator it = p.typeDeclaration.globalMatch(stripped.code);
    while (it.hasNext()) {
        const QString name = it.next().captured(2);
        if (!declared.contains(name)) {
            declared.insert(name);
            info.declares.append(name);
        }
    }
    QRegularExpression packageLine = p.packageDeclaration;
    packageLine.setPatternOptions(QRegularExpression::MultilineOption);
    const QRegularExpressionMatch package = packageLine.match(stripped.code);
    if (package.hasMatch())
        info.declares.append(QStringLiteral("package:") + package.captured(1));
    QSet<QString> referenced;
    QRegularExpressionMatchIterator refs = p.capitalized.globalMatch(stripped.code);
    while (refs.hasNext() && referenced.size() < 800) {
        const QString name = refs.next().captured(0);
        if (!declared.contains(name))
            referenced.insert(name);
    }
    info.references = QStringList(referenced.cbegin(), referenced.cend());
    std::sort(info.references.begin(), info.references.end());
}

bool generatedText(const QByteArray &bytes)
{
    const QByteArray head = bytes.left(2048).toLower();
    return head.contains("@generated") || head.contains("do not edit") ||
           head.contains("auto-generated") || head.contains("autogenerated") ||
           head.contains("generated by the protocol buffer") || head.contains("code generated by");
}

bool binaryText(const QByteArray &bytes)
{
    return bytes.left(8000).contains('\0');
}

Measured measureInternal(const QString &path, const QByteArray &bytes, bool withWindows)
{
    Measured measured;
    FileInfo &info = measured.info;
    info.path = path;
    const LanguageSpec spec = specFor(path);
    info.language = spec.name;
    info.bytes = bytes.size();
    info.test = isTestPath(path);
    const QString text = QString::fromUtf8(bytes);
    const Stripped stripped = stripSource(text, spec);
    const QStringList original = text.split(QLatin1Char('\n'));
    const QStringList withoutComments = stripped.noComments.split(QLatin1Char('\n'));
    const QStringList codeOnly = stripped.code.split(QLatin1Char('\n'));
    int count = int(original.size());
    if (count > 0 && original.last().isEmpty())
        --count;
    info.lines = count;
    for (int i = 0; i < count; ++i) {
        if (original.at(i).trimmed().isEmpty()) {
            ++info.blankLines;
            continue;
        }
        const QString nc = i < withoutComments.size() ? withoutComments.at(i).trimmed() : QString();
        if (nc.isEmpty()) {
            ++info.commentLines;
            continue;
        }
        if (spec.syntax == Syntax::Python && i < codeOnly.size()) {
            const QString code = codeOnly.at(i).trimmed();
            bool quotesOnly = !code.isEmpty();
            for (const QChar c : code) {
                if (c != QLatin1Char('"') && c != QLatin1Char('\'') && !c.isSpace()) {
                    quotesOnly = false;
                    break;
                }
            }
            if (quotesOnly) {
                ++info.commentLines;
                continue;
            }
        }
        ++info.codeLines;
    }
    switch (spec.syntax) {
    case Syntax::Braces:
        analyzeBraces(info, stripped);
        break;
    case Syntax::Python:
        analyzePython(info, stripped);
        break;
    case Syntax::Keyword:
        analyzeKeyword(info, stripped);
        break;
    case Syntax::Css:
        info.complexity = 0;
        break;
    }
    info.imports = importsFromStripped(spec.name, stripped.noComments);
    collectSymbols(info, stripped);
    if (withWindows && spec.syntax != Syntax::Css)
        computeWindows(measured, stripped.noComments);
    return measured;
}

}

namespace {

QString languageSpecName(const QString &language, LanguageSpec *out)
{
    for (auto it = languageTable().cbegin(); it != languageTable().cend(); ++it) {
        if (it.value().name == language) {
            if (out)
                *out = it.value();
            return it.key();
        }
    }
    return {};
}

const QSet<QString> &vendoredSegments()
{
    static const QSet<QString> segments = {
        QStringLiteral("node_modules"), QStringLiteral("vendor"),       QStringLiteral("vendors"),
        QStringLiteral("third_party"),  QStringLiteral("thirdparty"),   QStringLiteral("third-party"),
        QStringLiteral("bower_components"), QStringLiteral("pods"),     QStringLiteral("carthage"),
        QStringLiteral("deriveddata"),  QStringLiteral(".git"),         QStringLiteral("__pycache__"),
        QStringLiteral(".venv"),        QStringLiteral("venv"),         QStringLiteral("site-packages"),
        QStringLiteral(".tox"),         QStringLiteral(".dart_tool"),   QStringLiteral("godeps"),
        QStringLiteral(".next"),        QStringLiteral(".nuxt"),        QStringLiteral(".gradle"),
        QStringLiteral(".idea"),        QStringLiteral(".build"),       QStringLiteral("_build"),
        QStringLiteral(".cache"),       QStringLiteral("jspm_packages"), QStringLiteral(".yarn"),
        QStringLiteral(".pnpm-store")};
    return segments;
}

const QSet<QString> &buildSegments()
{
    static const QSet<QString> segments = {
        QStringLiteral("build"), QStringLiteral("dist"), QStringLiteral("out"),
        QStringLiteral("target"), QStringLiteral("coverage"), QStringLiteral("bin"),
        QStringLiteral("obj")};
    return segments;
}

QByteArray runGit(const QString &root, const QStringList &args, bool *ok, int timeoutMs,
                  const CancelFn &cancel, const QByteArray &input = {})
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process.setProcessEnvironment(environment);
    process.setProgram(QStringLiteral("git"));
    process.setArguments(QStringList{QStringLiteral("-C"), root, QStringLiteral("-c"),
                                     QStringLiteral("core.quotepath=off"), QStringLiteral("-c"),
                                     QStringLiteral("i18n.logOutputEncoding=UTF-8")} +
                         args);
    process.start();
    if (ok)
        *ok = false;
    if (!process.waitForStarted(10000))
        return {};
    if (!input.isEmpty()) {
        qsizetype written = 0;
        QByteArray collected;
        while (written < input.size()) {
            const qint64 chunk = process.write(input.constData() + written,
                                               qMin<qsizetype>(input.size() - written, 1 << 16));
            if (chunk < 0)
                break;
            written += chunk;
            process.waitForBytesWritten(1000);
            collected += process.readAllStandardOutput();
        }
        process.closeWriteChannel();
        QElapsedTimer timer;
        timer.start();
        while (process.state() != QProcess::NotRunning) {
            process.waitForReadyRead(200);
            collected += process.readAllStandardOutput();
            process.readAllStandardError();
            if ((cancel && cancel()) || timer.elapsed() > timeoutMs) {
                process.kill();
                process.waitForFinished(2000);
                return {};
            }
        }
        collected += process.readAllStandardOutput();
        if (ok)
            *ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        return collected;
    }
    process.closeWriteChannel();
    QByteArray output;
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        output += process.readAllStandardOutput();
        process.readAllStandardError();
        if ((cancel && cancel()) || timer.elapsed() > timeoutMs) {
            process.kill();
            process.waitForFinished(2000);
            return {};
        }
    }
    output += process.readAllStandardOutput();
    if (ok)
        *ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    return output;
}

QString gitText(const QString &root, const QStringList &args)
{
    bool ok = false;
    const QByteArray out = runGit(root, args, &ok, 15000, {});
    return ok ? QString::fromUtf8(out).trimmed() : QString();
}

struct Resolver {
    Snapshot &s;
    QHash<QString, QVector<int>> byName;
    QHash<QString, QVector<int>> byNoExt;
    QHash<QString, QVector<int>> dirFiles;
    QHash<QString, QVector<int>> packages;
    QSet<QString> namespaces;
    QHash<QString, QHash<QString, QVector<int>>> types;

    explicit Resolver(Snapshot &snapshot)
        : s(snapshot)
    {
        for (int i = 0; i < s.files.size(); ++i) {
            const FileInfo &file = s.files.at(i);
            byName[fileNameOf(file.path)].append(i);
            byNoExt[withoutExtension(file.path)].append(i);
            dirFiles[dirOf(file.path)].append(i);
            for (const QString &declared : file.declares) {
                if (declared.startsWith(QLatin1String("package:"))) {
                    const QString name = declared.mid(8);
                    packages[name].append(i);
                    namespaces.insert(name);
                    QString prefix = name;
                    while (prefix.contains(QLatin1Char('.'))) {
                        prefix = prefix.section(QLatin1Char('.'), 0, -2);
                        namespaces.insert(prefix);
                    }
                } else {
                    types[family(file.language)][declared].append(i);
                }
            }
        }
    }

    static QString family(const QString &language)
    {
        if (language == QLatin1String("Java") || language == QLatin1String("Kotlin") ||
            language == QLatin1String("Scala") || language == QLatin1String("Groovy"))
            return QStringLiteral("jvm");
        return language;
    }

    int exact(const QString &path) const
    {
        const QString clean = normalizedPath(path);
        return clean.isEmpty() ? -1 : s.byPath.value(clean, -1);
    }

    int probe(const QString &base, const QStringList &extensions) const
    {
        for (const QString &extension : extensions) {
            const int hit = exact(base + extension);
            if (hit >= 0)
                return hit;
        }
        return -1;
    }

    int best(const QVector<int> &candidates, const QString &importer) const
    {
        int chosen = -1;
        int score = -1;
        for (const int candidate : candidates) {
            const int common = commonPrefix(s.files.at(candidate).path, importer);
            if (common > score) {
                score = common;
                chosen = candidate;
            }
        }
        return chosen;
    }

    int suffix(const QString &spec, const QString &importer, const QStringList &extensions) const
    {
        const QString clean = QDir::cleanPath(spec);
        if (clean.isEmpty() || clean.startsWith(QLatin1String("..")))
            return -1;
        for (const QString &extension : extensions) {
            const QString wanted = clean + extension;
            const QVector<int> named = byName.value(fileNameOf(wanted));
            QVector<int> matches;
            for (const int candidate : named) {
                const QString &path = s.files.at(candidate).path;
                if (path == wanted || path.endsWith(QLatin1Char('/') + wanted))
                    matches.append(candidate);
            }
            if (!matches.isEmpty())
                return best(matches, importer);
        }
        return -1;
    }

    static const QStringList &jsExtensions()
    {
        static const QStringList list = {
            QString(), QStringLiteral(".ts"), QStringLiteral(".tsx"), QStringLiteral(".js"),
            QStringLiteral(".jsx"), QStringLiteral(".mjs"), QStringLiteral(".cjs"),
            QStringLiteral(".mts"), QStringLiteral(".cts"), QStringLiteral(".vue"),
            QStringLiteral(".svelte"), QStringLiteral(".d.ts"), QStringLiteral("/index.ts"),
            QStringLiteral("/index.tsx"), QStringLiteral("/index.js"), QStringLiteral("/index.jsx"),
            QStringLiteral("/index.mjs"), QStringLiteral("/index.vue")};
        return list;
    }

    QVector<int> resolve(int importer, const QString &rawSpec) const
    {
        const FileInfo &file = s.files.at(importer);
        const QString &language = file.language;
        const QString dir = dirOf(file.path);
        const QString spec = rawSpec.startsWith(QLatin1Char('?')) ? rawSpec.mid(1) : rawSpec;
        const auto one = [](int index) { return index >= 0 ? QVector<int>{index} : QVector<int>(); };
        if (language == QLatin1String("C") || language == QLatin1String("C++") ||
            language == QLatin1String("Objective-C")) {
            static const QStringList none = {QString()};
            if (spec.startsWith(QLatin1Char('<'))) {
                const QString inner = spec.mid(1, spec.size() - 2);
                if (!inner.contains(QLatin1Char('.')) && !inner.contains(QLatin1Char('/')))
                    return {};
                return one(suffix(inner, file.path, none));
            }
            int hit = exact(joinPath(dir, spec));
            if (hit < 0)
                hit = exact(spec);
            if (hit < 0)
                hit = suffix(spec, file.path, none);
            return one(hit);
        }
        if (language == QLatin1String("JavaScript") || language == QLatin1String("TypeScript") ||
            language == QLatin1String("Vue") || language == QLatin1String("Svelte")) {
            QStringList bases;
            if (spec.startsWith(QLatin1String("./")) || spec.startsWith(QLatin1String("../")) ||
                spec == QLatin1String(".") || spec == QLatin1String("..")) {
                bases << joinPath(dir, spec);
            } else if (spec.startsWith(QLatin1String("@/")) || spec.startsWith(QLatin1String("~/"))) {
                bases << QStringLiteral("src/") + spec.mid(2) << spec.mid(2);
            } else if (spec.startsWith(QLatin1Char('/'))) {
                bases << spec.mid(1);
            } else {
                bases << spec << QStringLiteral("src/") + spec;
            }
            for (const QString &base : std::as_const(bases)) {
                int hit = probe(base, jsExtensions());
                if (hit < 0) {
                    for (const char *js : {".js", ".mjs", ".jsx", ".cjs"}) {
                        if (base.endsWith(QLatin1String(js))) {
                            hit = probe(base.left(base.size() - qsizetype(qstrlen(js))),
                                        {QStringLiteral(".ts"), QStringLiteral(".tsx"),
                                         QStringLiteral(".mts"), QStringLiteral(".cts")});
                            break;
                        }
                    }
                }
                if (hit >= 0)
                    return one(hit);
            }
            if (!spec.startsWith(QLatin1Char('.')) && spec.contains(QLatin1Char('/')) &&
                !spec.startsWith(QLatin1Char('@')))
                return one(suffix(spec, file.path,
                                  {QStringLiteral(".ts"), QStringLiteral(".tsx"), QStringLiteral(".js"),
                                   QStringLiteral(".jsx"), QStringLiteral("/index.ts"),
                                   QStringLiteral("/index.js")}));
            return {};
        }
        if (language == QLatin1String("Python"))
            return one(resolvePython(file, spec));
        if (language == QLatin1String("Go")) {
            for (auto it = s.packageRoots.cbegin(); it != s.packageRoots.cend(); ++it) {
                if (!it.key().startsWith(QLatin1String("go:")))
                    continue;
                const QString module = it.key().mid(3);
                if (spec != module && !spec.startsWith(module + QLatin1Char('/')))
                    continue;
                const QString target = normalizedPath(joinPath(it.value(), spec.mid(module.size() + 1)));
                QVector<int> hits;
                for (const int candidate : dirFiles.value(target)) {
                    const FileInfo &other = s.files.at(candidate);
                    if (other.language == QLatin1String("Go") && !other.test)
                        hits.append(candidate);
                    if (hits.size() >= 16)
                        break;
                }
                return hits;
            }
            return {};
        }
        if (language == QLatin1String("Rust"))
            return one(resolveRust(file, spec));
        if (language == QLatin1String("Java") || language == QLatin1String("Kotlin") ||
            language == QLatin1String("Scala") || language == QLatin1String("Groovy")) {
            if (spec.endsWith(QLatin1String(".*")))
                return {};
            static const QStringList jvm = {QStringLiteral(".java"), QStringLiteral(".kt"),
                                            QStringLiteral(".scala"), QStringLiteral(".groovy")};
            QString current = spec;
            for (int attempt = 0; attempt < 2 && current.contains(QLatin1Char('.')); ++attempt) {
                const int hit = suffix(QString(current).replace(QLatin1Char('.'), QLatin1Char('/')),
                                       file.path, jvm);
                if (hit >= 0)
                    return one(hit);
                const QString package = current.section(QLatin1Char('.'), 0, -2);
                const QString type = current.section(QLatin1Char('.'), -1);
                QVector<int> declared;
                for (const int candidate : packages.value(package))
                    if (stemOf(s.files.at(candidate).path) == type ||
                        s.files.at(candidate).declares.contains(type))
                        declared.append(candidate);
                if (!declared.isEmpty())
                    return one(best(declared, file.path));
                current = package;
            }
            return {};
        }
        if (language == QLatin1String("PHP")) {
            if (spec.contains(QLatin1Char('\\'))) {
                QStringList parts = spec.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
                while (parts.size() >= 2) {
                    const int hit = suffix(parts.join(QLatin1Char('/')), file.path, {QStringLiteral(".php")});
                    if (hit >= 0)
                        return one(hit);
                    parts.removeFirst();
                }
                return {};
            }
            int hit = exact(joinPath(dir, spec));
            if (hit < 0)
                hit = exact(spec);
            return one(hit);
        }
        if (language == QLatin1String("Ruby")) {
            if (spec.startsWith(QLatin1String("./")))
                return one(probe(joinPath(dir, spec.mid(2)), {QString(), QStringLiteral(".rb")}));
            int hit = probe(QStringLiteral("lib/") + spec, {QStringLiteral(".rb")});
            if (hit < 0 && spec.contains(QLatin1Char('/')))
                hit = suffix(spec, file.path, {QStringLiteral(".rb")});
            return one(hit);
        }
        if (language == QLatin1String("Dart")) {
            if (spec.startsWith(QLatin1String("dart:")))
                return {};
            if (spec.startsWith(QLatin1String("package:"))) {
                const QString rest = spec.mid(8);
                const QString name = rest.section(QLatin1Char('/'), 0, 0);
                const auto root = s.packageRoots.constFind(QStringLiteral("dart:") + name);
                if (root == s.packageRoots.cend())
                    return {};
                return one(exact(joinPath(joinPath(root.value(), QStringLiteral("lib")),
                                          rest.section(QLatin1Char('/'), 1))));
            }
            return one(exact(joinPath(dir, spec)));
        }
        if (language == QLatin1String("Lua")) {
            const QString path = QString(spec).replace(QLatin1Char('.'), QLatin1Char('/'));
            int hit = probe(path, {QStringLiteral(".lua"), QStringLiteral("/init.lua")});
            if (hit < 0)
                hit = suffix(path, file.path, {QStringLiteral(".lua"), QStringLiteral("/init.lua")});
            return one(hit);
        }
        if (language == QLatin1String("CSS") || language == QLatin1String("SCSS") ||
            language == QLatin1String("Less")) {
            if (spec.contains(QLatin1String("://")))
                return {};
            const QString base = joinPath(dir, spec);
            const QString partial = joinPath(dirOf(base), QLatin1Char('_') + fileNameOf(base));
            int hit = probe(base, {QString(), QStringLiteral(".scss"), QStringLiteral(".sass"),
                                   QStringLiteral(".css"), QStringLiteral(".less")});
            if (hit < 0)
                hit = probe(partial, {QString(), QStringLiteral(".scss"), QStringLiteral(".sass")});
            return one(hit);
        }
        if (language == QLatin1String("QML")) {
            if (!spec.startsWith(QLatin1String("./")))
                return {};
            QVector<int> hits;
            for (const int candidate : dirFiles.value(normalizedPath(joinPath(dir, spec))))
                if (s.files.at(candidate).language == QLatin1String("QML") && hits.size() < 12)
                    hits.append(candidate);
            return hits;
        }
        if (language == QLatin1String("Shell")) {
            if (spec.contains(QLatin1Char('$')))
                return one(suffix(spec.section(QLatin1Char('/'), -2), file.path, {QString()}));
            int hit = exact(joinPath(dir, spec));
            if (hit < 0)
                hit = exact(spec);
            return one(hit);
        }
        return {};
    }

    int resolvePython(const FileInfo &file, const QString &spec) const
    {
        static const QStringList extensions = {QStringLiteral(".py"), QStringLiteral("/__init__.py"),
                                               QStringLiteral(".pyi")};
        int dots = 0;
        while (dots < spec.size() && spec.at(dots) == QLatin1Char('.'))
            ++dots;
        const QString module = spec.mid(dots);
        const QString path = QString(module).replace(QLatin1Char('.'), QLatin1Char('/'));
        if (dots > 0) {
            QString base = dirOf(file.path);
            for (int up = 1; up < dots; ++up)
                base = dirOf(base);
            if (path.isEmpty())
                return exact(joinPath(base, QStringLiteral("__init__.py")));
            return probe(joinPath(base, path), extensions);
        }
        if (path.isEmpty())
            return -1;
        int hit = probe(path, extensions);
        if (hit < 0)
            hit = probe(joinPath(dirOf(file.path), path), extensions);
        if (hit < 0) {
            QVector<int> matches;
            for (const QString &tail : {path, path + QStringLiteral("/__init__")}) {
                const QString name = fileNameOf(tail);
                for (const int candidate : byName.value(name + QStringLiteral(".py"))) {
                    const QString noExt = withoutExtension(s.files.at(candidate).path);
                    if (noExt == tail || noExt.endsWith(QLatin1Char('/') + tail))
                        matches.append(candidate);
                }
            }
            if (!matches.isEmpty())
                hit = best(matches, file.path);
        }
        return hit;
    }

    int resolveRustPath(const QString &base, const QStringList &segments) const
    {
        for (int k = int(segments.size()); k >= 1; --k) {
            const QString joined = joinPath(base, segments.mid(0, k).join(QLatin1Char('/')));
            const int hit = probe(joined, {QStringLiteral(".rs"), QStringLiteral("/mod.rs")});
            if (hit >= 0)
                return hit;
        }
        return -1;
    }

    int resolveRust(const FileInfo &file, const QString &spec) const
    {
        const QString dir = dirOf(file.path);
        const QString stem = stemOf(file.path);
        const bool rootLike = stem == QLatin1String("mod") || stem == QLatin1String("lib") ||
                              stem == QLatin1String("main");
        const QString moduleDir = rootLike ? dir : joinPath(dir, stem);
        if (spec.startsWith(QLatin1String("mod "))) {
            const QString name = spec.mid(4);
            return probe(joinPath(moduleDir, name), {QStringLiteral(".rs"), QStringLiteral("/mod.rs")});
        }
        QStringList segments = spec.split(QStringLiteral("::"));
        const QString head = segments.takeFirst();
        if (head == QLatin1String("crate")) {
            QString crate = dir;
            while (true) {
                if (exact(joinPath(crate, QStringLiteral("lib.rs"))) >= 0 ||
                    exact(joinPath(crate, QStringLiteral("main.rs"))) >= 0)
                    break;
                if (crate.isEmpty())
                    return -1;
                crate = dirOf(crate);
            }
            return resolveRustPath(crate, segments);
        }
        if (head == QLatin1String("super"))
            return resolveRustPath(rootLike ? dirOf(dir) : dir, segments);
        return resolveRustPath(moduleDir, segments);
    }
};

double clamp01(double value)
{
    return qBound(0.0, value, 1.0);
}

}

int Snapshot::totalCodeLines() const
{
    qint64 total = 0;
    for (const FileInfo &file : files)
        total += file.codeLines;
    return int(qMin<qint64>(total, std::numeric_limits<int>::max()));
}

int Snapshot::totalComplexity() const
{
    qint64 total = 0;
    for (const FileInfo &file : files)
        total += file.complexity;
    return int(qMin<qint64>(total, std::numeric_limits<int>::max()));
}

QList<Coupling> Snapshot::couplingsFor(int file) const
{
    QList<Coupling> result;
    for (const Coupling &coupling : couplings) {
        if (coupling.a == file) {
            result.append(coupling);
        } else if (coupling.b == file) {
            Coupling swapped = coupling;
            swapped.a = coupling.b;
            swapped.b = coupling.a;
            result.append(swapped);
        }
    }
    std::sort(result.begin(), result.end(), [](const Coupling &x, const Coupling &y) {
        if (x.degree != y.degree)
            return x.degree > y.degree;
        return x.shared > y.shared;
    });
    return result;
}

int ModuleGraph::edgeIndex(int from, int to) const
{
    for (int i = 0; i < edges.size(); ++i)
        if (edges.at(i).from == from && edges.at(i).to == to)
            return i;
    return -1;
}

QString languageForPath(const QString &path)
{
    return specFor(path).name;
}

bool isTestPath(const QString &path)
{
    static const QSet<QString> testSegments = {
        QStringLiteral("test"),   QStringLiteral("tests"),   QStringLiteral("__tests__"),
        QStringLiteral("spec"),   QStringLiteral("specs"),   QStringLiteral("testing"),
        QStringLiteral("androidtest"), QStringLiteral("uitests"), QStringLiteral("e2e"),
        QStringLiteral("__mocks__"), QStringLiteral("fixtures"), QStringLiteral("testdata"),
        QStringLiteral("browser_tests")};
    const QStringList segments = path.split(QLatin1Char('/'));
    for (int i = 0; i + 1 < segments.size(); ++i) {
        const QString segment = segments.at(i).toLower();
        if (testSegments.contains(segment) || segment.endsWith(QLatin1String("tests")))
            return true;
    }
    const QString name = segments.last();
    const QString lower = name.toLower();
    if (lower.startsWith(QLatin1String("test_")) || lower.contains(QLatin1String("_test.")) ||
        lower.contains(QLatin1String(".test.")) || lower.contains(QLatin1String(".spec.")) ||
        lower.contains(QLatin1String("_spec.")) || lower.startsWith(QLatin1String("conftest.")))
        return true;
    thread_local const QRegularExpression typedTest(QStringLiteral("[a-z0-9]Tests?\\.(swift|kt|java|cs|m|mm|scala|dart)$"));
    return typedTest.match(name).hasMatch();
}

bool isVendoredPath(const QString &path)
{
    const QStringList segments = path.split(QLatin1Char('/'));
    for (int i = 0; i + 1 < segments.size(); ++i)
        if (vendoredSegments().contains(segments.at(i).toLower()))
            return true;
    const QString name = segments.last().toLower();
    static const char *suffixes[] = {".min.js", ".min.css", ".bundle.js", ".pb.go", "_pb2.py",
                                     "_pb2_grpc.py", ".g.dart", ".freezed.dart", ".designer.cs",
                                     ".pb.h", ".pb.cc", ".pb.swift", ".chunk.js", "-min.js"};
    for (const char *suffix : suffixes)
        if (name.endsWith(QLatin1String(suffix)))
            return true;
    return name.contains(QLatin1String(".generated.")) || name == QLatin1String("package-lock.json");
}

FileInfo measureSource(const QString &path, const QByteArray &bytes)
{
    return measureInternal(path, bytes, false).info;
}

QStringList extractImports(const QString &language, const QString &text)
{
    LanguageSpec spec;
    if (languageSpecName(language, &spec).isEmpty())
        return {};
    QStringList result;
    for (const QString &value : importsFromStripped(language, stripSource(text, spec).noComments))
        if (!value.startsWith(QLatin1Char('?')))
            result.append(value);
    return result;
}

double computeHealth(const FileInfo &file, QStringList *issues)
{
    double score = 10.0;
    QStringList reasons;
    const QLocale locale = QLocale::c();
    int brainCount = 0;
    double brain = 0;
    double complex = 0;
    double longFunctions = 0;
    double arguments = 0;
    QStringList brainNames;
    QStringList complexNames;
    QStringList longNames;
    QStringList argumentNames;
    const FunctionInfo *deepest = nullptr;
    for (const FunctionInfo &fn : file.functions) {
        const bool isBrain = fn.length >= 70 && fn.complexity >= 15;
        if (isBrain) {
            ++brainCount;
            brain += 1.5;
            brainNames << QStringLiteral("%1 (complexity %2, %3 lines)").arg(fn.name).arg(fn.complexity).arg(fn.length);
        } else if (fn.complexity > 10) {
            complex += 0.35;
            complexNames << QStringLiteral("%1 (%2)").arg(fn.name).arg(fn.complexity);
        }
        if (!isBrain && fn.length > 70) {
            longFunctions += 0.25;
            longNames << QStringLiteral("%1 (%2 lines)").arg(fn.name).arg(fn.length);
        }
        if (fn.parameters > 5) {
            arguments += 0.2;
            argumentNames << QStringLiteral("%1 (%2)").arg(fn.name).arg(fn.parameters);
        }
        if (!deepest || fn.nesting > deepest->nesting)
            deepest = &fn;
    }
    const auto listed = [](const QStringList &names) {
        QStringList shown = names.mid(0, 3);
        QString text = shown.join(QStringLiteral(", "));
        if (names.size() > 3)
            text += QStringLiteral(" and %1 more").arg(names.size() - 3);
        return text;
    };
    if (brainCount > 0) {
        score -= qMin(3.5, brain);
        reasons << QStringLiteral("Brain method%1: %2").arg(brainCount == 1 ? QString() : QStringLiteral("s"),
                                                            listed(brainNames));
    }
    if (!complexNames.isEmpty()) {
        score -= qMin(2.5, complex);
        reasons << QStringLiteral("Complex function%1: %2")
                       .arg(complexNames.size() == 1 ? QString() : QStringLiteral("s"), listed(complexNames));
    }
    if (file.maxComplexity > 50) {
        score -= 1.0;
        reasons << QStringLiteral("Extremely complex function (complexity %1)").arg(file.maxComplexity);
    } else if (file.maxComplexity > 25) {
        score -= 0.5;
    }
    if (deepest && deepest->nesting >= 4) {
        const double penalty = deepest->nesting >= 6 ? 1.25 : 0.5;
        score -= penalty;
        reasons << QStringLiteral("Deeply nested logic: depth %1 in %2").arg(deepest->nesting).arg(deepest->name);
    }
    if (!longNames.isEmpty()) {
        score -= qMin(1.5, longFunctions);
        reasons << QStringLiteral("Large function%1: %2")
                       .arg(longNames.size() == 1 ? QString() : QStringLiteral("s"), listed(longNames));
    }
    if (!argumentNames.isEmpty()) {
        score -= qMin(1.0, arguments);
        reasons << QStringLiteral("Many arguments: %1").arg(listed(argumentNames));
    }
    if (file.codeLines > 800) {
        score -= qMin(2.0, (file.codeLines - 800) / 1000.0 + 0.5);
        reasons << QStringLiteral("Large file: %1 lines of code").arg(locale.toString(file.codeLines));
    }
    if (file.functions.size() > 75) {
        score -= 0.75;
        reasons << QStringLiteral("Low cohesion: %1 functions in one file").arg(file.functions.size());
    }
    if (file.codeLines > 0) {
        const double share = double(file.duplicatedLines) / double(file.codeLines);
        if (share > 0.1 && file.duplicatedLines >= 12) {
            score -= qMin(1.5, share * 5.0);
            reasons << QStringLiteral("Code duplication: %1 lines (%2 %)")
                           .arg(file.duplicatedLines)
                           .arg(int(std::lround(share * 100)));
        }
    }
    if (file.codeLines > 100 && double(file.complexity) / double(file.codeLines) > 0.25) {
        score -= 0.5;
        reasons << QStringLiteral("Complex conditionals: %1 decisions in %2 lines")
                       .arg(file.complexity)
                       .arg(file.codeLines);
    }
    if (file.dependsOn.size() > 25) {
        score -= 0.5;
        reasons << QStringLiteral("Many dependencies: imports %1 files").arg(file.dependsOn.size());
    }
    if (file.cycle >= 0) {
        score -= 0.5;
        reasons << QStringLiteral("Part of a circular import");
    }
    score = qBound(1.0, std::round(score * 10.0) / 10.0, 10.0);
    if (issues)
        *issues = reasons;
    return score;
}

HealthBand healthBand(double health)
{
    if (health >= 8.0)
        return HealthBand::Healthy;
    if (health >= 4.0)
        return HealthBand::Problematic;
    return HealthBand::Unhealthy;
}

QString healthBandName(HealthBand band)
{
    switch (band) {
    case HealthBand::Healthy:
        return QStringLiteral("Healthy");
    case HealthBand::Problematic:
        return QStringLiteral("Problematic");
    case HealthBand::Unhealthy:
        return QStringLiteral("Unhealthy");
    }
    return {};
}

void resolveDependencies(Snapshot &snapshot)
{
    Resolver resolver(snapshot);
    for (int i = 0; i < snapshot.files.size(); ++i) {
        FileInfo &file = snapshot.files[i];
        QSet<int> targets;
        QStringList shown;
        QStringList external;
        for (const QString &spec : std::as_const(file.imports)) {
            const bool optional = spec.startsWith(QLatin1Char('?'));
            const QVector<int> hits = resolver.resolve(i, spec);
            for (const int hit : hits)
                if (hit != i)
                    targets.insert(hit);
            if (optional)
                continue;
            shown.append(spec);
            if (hits.isEmpty()) {
                if (file.language == QLatin1String("C#") && resolver.namespaces.contains(spec))
                    continue;
                if (Resolver::family(file.language) == QLatin1String("jvm") &&
                    (resolver.namespaces.contains(spec.section(QLatin1Char('.'), 0, -2)) ||
                     resolver.namespaces.contains(QString(spec).remove(QStringLiteral(".*")))))
                    continue;
                external.append(spec);
            }
        }
        const QString family = Resolver::family(file.language);
        const auto typesIt = resolver.types.constFind(family);
        if (typesIt != resolver.types.cend()) {
            for (const QString &reference : std::as_const(file.references)) {
                const auto declared = typesIt->constFind(reference);
                if (declared == typesIt->cend() || declared->size() > 2)
                    continue;
                for (const int hit : *declared)
                    if (hit != i)
                        targets.insert(hit);
            }
        }
        file.references.clear();
        file.imports = shown;
        file.externalImports = external;
        file.dependsOn = QVector<int>(targets.cbegin(), targets.cend());
        std::sort(file.dependsOn.begin(), file.dependsOn.end());
        file.dependedOnBy.clear();
    }
    for (int i = 0; i < snapshot.files.size(); ++i)
        for (const int target : std::as_const(snapshot.files.at(i).dependsOn))
            snapshot.files[target].dependedOnBy.append(i);
}

QList<QVector<int>> stronglyConnected(const QList<QVector<int>> &adjacency)
{
    const int n = int(adjacency.size());
    QVector<int> index(n, -1);
    QVector<int> low(n, 0);
    QVector<bool> onStack(n, false);
    QVector<int> stack;
    QList<QVector<int>> components;
    int counter = 0;
    struct Frame {
        int node;
        int edge;
    };
    for (int start = 0; start < n; ++start) {
        if (index[start] >= 0)
            continue;
        QVector<Frame> work;
        work.append({start, 0});
        index[start] = low[start] = counter++;
        stack.append(start);
        onStack[start] = true;
        while (!work.isEmpty()) {
            Frame &frame = work.last();
            const QVector<int> &edges = adjacency.at(frame.node);
            if (frame.edge < edges.size()) {
                const int next = edges.at(frame.edge++);
                if (next < 0 || next >= n)
                    continue;
                if (index[next] < 0) {
                    index[next] = low[next] = counter++;
                    stack.append(next);
                    onStack[next] = true;
                    work.append({next, 0});
                } else if (onStack[next]) {
                    low[frame.node] = qMin(low[frame.node], index[next]);
                }
                continue;
            }
            const int node = frame.node;
            work.removeLast();
            if (!work.isEmpty())
                low[work.last().node] = qMin(low[work.last().node], low[node]);
            if (low[node] == index[node]) {
                QVector<int> component;
                while (true) {
                    const int member = stack.takeLast();
                    onStack[member] = false;
                    component.append(member);
                    if (member == node)
                        break;
                }
                std::sort(component.begin(), component.end());
                components.append(component);
            }
        }
    }
    return components;
}

void markCycles(Snapshot &snapshot)
{
    QList<QVector<int>> adjacency;
    adjacency.reserve(snapshot.files.size());
    for (const FileInfo &file : std::as_const(snapshot.files))
        adjacency.append(file.dependsOn);
    QList<QVector<int>> cycles;
    for (const QVector<int> &component : stronglyConnected(adjacency))
        if (component.size() > 1)
            cycles.append(component);
    std::sort(cycles.begin(), cycles.end(), [](const QVector<int> &a, const QVector<int> &b) {
        if (a.size() != b.size())
            return a.size() > b.size();
        return a.first() < b.first();
    });
    for (FileInfo &file : snapshot.files)
        file.cycle = -1;
    for (int c = 0; c < cycles.size(); ++c)
        for (const int member : std::as_const(cycles.at(c)))
            snapshot.files[member].cycle = c;
    snapshot.cycles = cycles;
}

namespace {

void applyWindows(Snapshot &snapshot, const QVector<QVector<int>> &significant,
                  const QVector<QVector<quint64>> &windows)
{
    QHash<quint64, int> counts;
    qint64 totalWindows = 0;
    for (const QVector<quint64> &list : windows)
        totalWindows += list.size();
    if (totalWindows > 6000000)
        return;
    counts.reserve(int(totalWindows));
    for (const QVector<quint64> &list : windows)
        for (const quint64 hash : list)
            ++counts[hash];
    for (int i = 0; i < snapshot.files.size() && i < windows.size(); ++i) {
        const QVector<quint64> &list = windows.at(i);
        const QVector<int> &lines = significant.at(i);
        QSet<int> marked;
        for (int k = 0; k < list.size(); ++k) {
            if (counts.value(list.at(k)) < 2)
                continue;
            for (int j = 0; j < kWindow && k + j < lines.size(); ++j)
                marked.insert(lines.at(k + j));
        }
        snapshot.files[i].duplicatedLines = int(marked.size());
    }
}

}

void markDuplication(Snapshot &snapshot, const QHash<QString, QByteArray> &texts)
{
    QVector<QVector<int>> significant(snapshot.files.size());
    QVector<QVector<quint64>> windows(snapshot.files.size());
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const auto it = texts.constFind(snapshot.files.at(i).path);
        if (it == texts.cend())
            continue;
        Measured measured = measureInternal(snapshot.files.at(i).path, it.value(), true);
        significant[i] = measured.significant;
        windows[i] = measured.windows;
    }
    applyWindows(snapshot, significant, windows);
}

void computeHotspots(Snapshot &snapshot)
{
    int maxCommits = 0;
    int maxLines = 1;
    for (const FileInfo &file : std::as_const(snapshot.files)) {
        maxCommits = qMax(maxCommits, file.commits);
        maxLines = qMax(maxLines, file.codeLines);
    }
    double top = 0;
    for (FileInfo &file : snapshot.files) {
        if (maxCommits <= 0 || file.commits <= 0) {
            file.hotspot = 0;
            continue;
        }
        const double change = double(file.commits) / double(maxCommits);
        const double size = std::sqrt(double(qMax(0, file.codeLines))) / std::sqrt(double(maxLines));
        file.hotspot = std::pow(change, 0.7) * std::pow(qMax(0.02, size), 0.3);
        top = qMax(top, file.hotspot);
    }
    if (top > 0)
        for (FileInfo &file : snapshot.files)
            file.hotspot = clamp01(file.hotspot / top);
}

void applyHistory(Snapshot &snapshot, const QByteArray &gitLog, const Options &options, qint64 now)
{
    struct Accumulator {
        int commits = 0;
        int recent = 0;
        int fixes = 0;
        int added = 0;
        int deleted = 0;
        qint64 last = 0;
        qint64 first = 0;
        QHash<QString, int> authorLines;
        QHash<QString, int> authorCommits;
    };
    thread_local const QRegularExpression fixPattern(
        QStringLiteral("\\b(fix|fixes|fixed|fixing|bug|bugs|bugfix|hotfix|defect|crash|crashes|"
                       "regression|broken|revert)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    QHash<int, Accumulator> perFile;
    QHash<QString, qint64> authorLast;
    QHash<QString, int> authorTotals;
    QHash<quint64, int> pairs;
    const qint64 recentCutoff = now - 90 * 86400;
    const qint64 formerCutoff = now - 180 * 86400;
    int commits = 0;
    qint64 earliest = 0;
    qint64 latest = 0;
    const QList<QByteArray> records = gitLog.split('\x1e');
    for (const QByteArray &record : records) {
        if (record.trimmed().isEmpty())
            continue;
        const int headerEnd = int(record.indexOf('\n'));
        const QByteArray header = headerEnd < 0 ? record : record.left(headerEnd);
        const QList<QByteArray> fields = header.split('\x1f');
        if (fields.size() < 3)
            continue;
        const QString author = QString::fromUtf8(fields.at(1)).trimmed();
        const qint64 time = fields.at(2).trimmed().toLongLong();
        const QString subject = fields.size() > 3 ? QString::fromUtf8(fields.at(3)) : QString();
        ++commits;
        earliest = earliest == 0 ? time : qMin(earliest, time);
        latest = qMax(latest, time);
        authorLast[author] = qMax(authorLast.value(author), time);
        ++authorTotals[author];
        const bool fix = fixPattern.match(subject).hasMatch();
        QVector<int> touched;
        if (headerEnd >= 0) {
            const QList<QByteArray> lines = record.mid(headerEnd + 1).split('\n');
            for (const QByteArray &line : lines) {
                const QList<QByteArray> parts = line.split('\t');
                if (parts.size() < 3)
                    continue;
                const QString path = QString::fromUtf8(parts.mid(2).join('\t')).trimmed();
                const int index = snapshot.byPath.value(path, -1);
                if (index < 0)
                    continue;
                const int added = parts.at(0) == "-" ? 0 : parts.at(0).toInt();
                const int deleted = parts.at(1) == "-" ? 0 : parts.at(1).toInt();
                Accumulator &acc = perFile[index];
                ++acc.commits;
                if (time >= recentCutoff)
                    ++acc.recent;
                if (fix)
                    ++acc.fixes;
                acc.added += added;
                acc.deleted += deleted;
                acc.last = qMax(acc.last, time);
                acc.first = acc.first == 0 ? time : qMin(acc.first, time);
                acc.authorLines[author] += added;
                ++acc.authorCommits[author];
                if (!touched.contains(index))
                    touched.append(index);
            }
        }
        if (touched.size() >= 2 && touched.size() <= options.maxCommitFiles) {
            std::sort(touched.begin(), touched.end());
            for (int a = 0; a < touched.size(); ++a)
                for (int b = a + 1; b < touched.size(); ++b)
                    ++pairs[(quint64(quint32(touched.at(a))) << 32) | quint32(touched.at(b))];
        }
    }
    QSet<QString> former;
    for (auto it = authorLast.cbegin(); it != authorLast.cend(); ++it)
        if (it.value() < formerCutoff)
            former.insert(it.key());
    for (auto it = perFile.cbegin(); it != perFile.cend(); ++it) {
        FileInfo &file = snapshot.files[it.key()];
        const Accumulator &acc = it.value();
        file.commits = acc.commits;
        file.recentCommits = acc.recent;
        file.fixCommits = acc.fixes;
        file.linesAdded = acc.added;
        file.linesDeleted = acc.deleted;
        file.lastChanged = acc.last;
        file.firstChanged = acc.first;
        file.authors = int(acc.authorCommits.size());
        int totalLines = 0;
        int formerLines = 0;
        QString main;
        int mainLines = -1;
        int mainCommits = -1;
        for (auto author = acc.authorCommits.cbegin(); author != acc.authorCommits.cend(); ++author) {
            const int lines = acc.authorLines.value(author.key());
            totalLines += lines;
            if (former.contains(author.key()))
                formerLines += lines;
            if (lines > mainLines || (lines == mainLines && author.value() > mainCommits) ||
                (lines == mainLines && author.value() == mainCommits && author.key() < main)) {
                main = author.key();
                mainLines = lines;
                mainCommits = author.value();
            }
        }
        file.mainAuthor = main;
        if (totalLines > 0) {
            file.mainAuthorShare = double(mainLines) / double(totalLines);
            file.formerAuthorShare = double(formerLines) / double(totalLines);
        } else if (acc.commits > 0) {
            file.mainAuthorShare = double(mainCommits) / double(acc.commits);
            int formerCommits = 0;
            for (auto author = acc.authorCommits.cbegin(); author != acc.authorCommits.cend(); ++author)
                if (former.contains(author.key()))
                    formerCommits += author.value();
            file.formerAuthorShare = double(formerCommits) / double(acc.commits);
        }
        QList<QPair<QString, int>> authors;
        for (auto author = acc.authorCommits.cbegin(); author != acc.authorCommits.cend(); ++author)
            authors.append({author.key(), author.value()});
        std::sort(authors.begin(), authors.end(), [](const auto &x, const auto &y) {
            if (x.second != y.second)
                return x.second > y.second;
            return x.first < y.first;
        });
        file.topAuthors = authors.mid(0, 5);
    }
    QList<Coupling> couplings;
    for (auto it = pairs.cbegin(); it != pairs.cend(); ++it) {
        if (it.value() < options.minSharedCommits)
            continue;
        const int a = int(it.key() >> 32);
        const int b = int(it.key() & 0xffffffffu);
        const double average = (snapshot.files.at(a).commits + snapshot.files.at(b).commits) / 2.0;
        if (average <= 0)
            continue;
        const double degree = clamp01(it.value() / average);
        if (degree < options.minCouplingDegree)
            continue;
        couplings.append({a, b, it.value(), degree});
    }
    std::sort(couplings.begin(), couplings.end(), [](const Coupling &x, const Coupling &y) {
        if (x.degree != y.degree)
            return x.degree > y.degree;
        if (x.shared != y.shared)
            return x.shared > y.shared;
        return x.a < y.a;
    });
    if (couplings.size() > 20000)
        couplings = couplings.mid(0, 20000);
    snapshot.couplings = couplings;
    QList<QPair<QString, int>> everyone;
    for (auto it = authorTotals.cbegin(); it != authorTotals.cend(); ++it)
        everyone.append({it.key(), it.value()});
    std::sort(everyone.begin(), everyone.end(), [](const auto &x, const auto &y) {
        if (x.second != y.second)
            return x.second > y.second;
        return x.first < y.first;
    });
    snapshot.authors.clear();
    snapshot.formerAuthors.clear();
    for (const auto &entry : std::as_const(everyone)) {
        snapshot.authors.append(entry.first);
        if (former.contains(entry.first))
            snapshot.formerAuthors.append(entry.first);
    }
    snapshot.commitsScanned = commits;
    snapshot.historyStart = earliest;
    snapshot.historyEnd = latest;
    if (commits > 0) {
        const qint64 span = qMax<qint64>(1, (now - earliest + 86399) / 86400);
        snapshot.historyDays = int(qMin<qint64>(span, options.historyDays));
    } else {
        snapshot.historyDays = 0;
    }
}

QString modulePathFor(const QString &filePath, int depth)
{
    const QStringList segments = filePath.split(QLatin1Char('/'));
    const int dirs = int(segments.size()) - 1;
    const int take = qMin(qMax(1, depth), dirs);
    if (take <= 0)
        return {};
    return segments.mid(0, take).join(QLatin1Char('/'));
}

int autoModuleDepth(const Snapshot &snapshot)
{
    int previousCount = 0;
    int bestDepth = 1;
    int bestCount = 0;
    for (int depth = 1; depth <= 6; ++depth) {
        QSet<QString> modules;
        for (const FileInfo &file : snapshot.files)
            modules.insert(modulePathFor(file.path, depth));
        const int count = int(modules.size());
        if (count >= 8 && count <= 60)
            return depth;
        if (count > 60)
            return previousCount >= 3 ? depth - 1 : (count <= 140 ? depth : qMax(1, depth - 1));
        if (count > bestCount) {
            bestCount = count;
            bestDepth = depth;
        }
        if (count == previousCount && depth > 1)
            break;
        previousCount = count;
    }
    return qMax(1, bestDepth);
}

ModuleGraph buildModuleGraph(const Snapshot &snapshot, int depth)
{
    ModuleGraph graph;
    graph.depth = depth > 0 ? depth : autoModuleDepth(snapshot);
    QHash<QString, int> byPath;
    graph.moduleOfFile.resize(snapshot.files.size());
    QStringList paths;
    for (const FileInfo &file : snapshot.files) {
        const QString module = modulePathFor(file.path, graph.depth);
        if (!byPath.contains(module)) {
            byPath.insert(module, -1);
            paths.append(module);
        }
    }
    std::sort(paths.begin(), paths.end());
    for (const QString &path : std::as_const(paths)) {
        ModuleNode node;
        node.path = path;
        node.name = path.isEmpty() ? QStringLiteral("(root files)") : path;
        byPath[path] = int(graph.nodes.size());
        graph.nodes.append(node);
    }
    QVector<double> weightedHealth(graph.nodes.size(), 0);
    QVector<double> weights(graph.nodes.size(), 0);
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const FileInfo &file = snapshot.files.at(i);
        const int module = byPath.value(modulePathFor(file.path, graph.depth));
        graph.moduleOfFile[i] = module;
        ModuleNode &node = graph.nodes[module];
        node.files.append(i);
        node.codeLines += file.codeLines;
        node.complexity += file.complexity;
        node.functions += int(file.functions.size());
        node.types += file.types;
        node.abstractTypes += file.abstractTypes;
        node.commits += file.commits;
        const double weight = qMax(1, file.codeLines);
        weightedHealth[module] += file.health * weight;
        weights[module] += weight;
    }
    QHash<quint64, int> edgeIndex;
    QVector<QSet<int>> incomingFiles(graph.nodes.size());
    QVector<QSet<int>> outgoingFiles(graph.nodes.size());
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const int from = graph.moduleOfFile.at(i);
        for (const int target : snapshot.files.at(i).dependsOn) {
            const int to = graph.moduleOfFile.at(target);
            if (from == to) {
                ++graph.nodes[from].internalEdges;
                continue;
            }
            const quint64 key = (quint64(quint32(from)) << 32) | quint32(to);
            auto it = edgeIndex.find(key);
            if (it == edgeIndex.end()) {
                ModuleEdge edge;
                edge.from = from;
                edge.to = to;
                it = edgeIndex.insert(key, int(graph.edges.size()));
                graph.edges.append(edge);
            }
            ModuleEdge &edge = graph.edges[it.value()];
            ++edge.weight;
            edge.fileEdges.append({i, target});
            outgoingFiles[from].insert(target);
            incomingFiles[to].insert(i);
        }
    }
    QList<QVector<int>> adjacency;
    for (int m = 0; m < graph.nodes.size(); ++m)
        adjacency.append(QVector<int>());
    for (const ModuleEdge &edge : std::as_const(graph.edges))
        adjacency[edge.from].append(edge.to);
    const QList<QVector<int>> components = stronglyConnected(adjacency);
    QVector<int> componentOf(graph.nodes.size(), -1);
    for (int c = 0; c < components.size(); ++c)
        for (const int member : components.at(c))
            componentOf[member] = c;
    for (const QVector<int> &component : components)
        if (component.size() > 1)
            graph.cycles.append(component);
    std::sort(graph.cycles.begin(), graph.cycles.end(), [](const QVector<int> &a, const QVector<int> &b) {
        return a.size() > b.size();
    });
    for (int c = 0; c < graph.cycles.size(); ++c)
        for (const int member : graph.cycles.at(c))
            graph.nodes[member].cycle = c;
    for (ModuleEdge &edge : graph.edges)
        edge.inCycle = componentOf[edge.from] == componentOf[edge.to];
    const int componentCount = int(components.size());
    QVector<QSet<int>> componentOut(componentCount);
    QVector<QSet<int>> componentIn(componentCount);
    for (const ModuleEdge &edge : std::as_const(graph.edges)) {
        const int a = componentOf[edge.from];
        const int b = componentOf[edge.to];
        if (a != b) {
            componentOut[a].insert(b);
            componentIn[b].insert(a);
        }
    }
    QVector<int> remaining(componentCount);
    QVector<int> layer(componentCount, 0);
    QVector<int> queue;
    for (int c = 0; c < componentCount; ++c) {
        remaining[c] = int(componentOut[c].size());
        if (remaining[c] == 0)
            queue.append(c);
    }
    for (int head = 0; head < queue.size(); ++head) {
        const int c = queue.at(head);
        for (const int predecessor : std::as_const(componentIn[c])) {
            layer[predecessor] = qMax(layer[predecessor], layer[c] + 1);
            if (--remaining[predecessor] == 0)
                queue.append(predecessor);
        }
    }
    for (int m = 0; m < graph.nodes.size(); ++m) {
        ModuleNode &node = graph.nodes[m];
        node.layer = layer[componentOf[m]];
        node.afferent = int(incomingFiles[m].size());
        node.efferent = int(outgoingFiles[m].size());
        const int total = node.afferent + node.efferent;
        node.instability = total > 0 ? double(node.efferent) / double(total) : 0.0;
        node.abstractness = node.types > 0 ? double(node.abstractTypes) / double(node.types) : 0.0;
        node.distance = std::abs(node.abstractness + node.instability - 1.0);
        node.cohesion = node.files.isEmpty() ? 0.0
                                             : (node.internalEdges + 1.0) / double(node.files.size());
        node.health = weights[m] > 0 ? std::round(weightedHealth[m] / weights[m] * 10.0) / 10.0 : 10.0;
    }
    return graph;
}

namespace {

struct Listed {
    QString path;
    QString object;
    qint64 size = -1;
};

QList<Listed> listFiles(const QString &root, bool git, bool bare, int *total, const CancelFn &cancel)
{
    QList<Listed> files;
    if (git && bare) {
        bool ok = false;
        const QByteArray out = runGit(root, {QStringLiteral("ls-tree"), QStringLiteral("-r"),
                                             QStringLiteral("-z"), QStringLiteral("-l"),
                                             QStringLiteral("HEAD")},
                                      &ok, 120000, cancel);
        for (const QByteArray &entry : out.split('\0')) {
            const int tab = int(entry.indexOf('\t'));
            if (tab < 0)
                continue;
            const QList<QByteArray> meta = entry.left(tab).simplified().split(' ');
            if (meta.size() < 4 || meta.at(1) != "blob")
                continue;
            files.append({QString::fromUtf8(entry.mid(tab + 1)), QString::fromLatin1(meta.at(2)),
                          meta.at(3).toLongLong()});
        }
    } else if (git) {
        bool ok = false;
        const QByteArray out = runGit(root, {QStringLiteral("ls-files"), QStringLiteral("-z"),
                                             QStringLiteral("--cached"), QStringLiteral("--others"),
                                             QStringLiteral("--exclude-standard")},
                                      &ok, 120000, cancel);
        QSet<QString> seen;
        for (const QByteArray &entry : out.split('\0')) {
            if (entry.isEmpty())
                continue;
            const QString path = QString::fromUtf8(entry);
            if (seen.contains(path))
                continue;
            seen.insert(path);
            files.append({path, QString(), -1});
        }
    } else {
        const QDir base(root);
        QDirIterator it(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString absolute = it.next();
            const QString relative = base.relativeFilePath(absolute);
            const QStringList segments = relative.split(QLatin1Char('/'));
            bool skip = false;
            for (int i = 0; i + 1 < segments.size(); ++i) {
                const QString segment = segments.at(i).toLower();
                if (segment.startsWith(QLatin1Char('.')) || buildSegments().contains(segment) ||
                    vendoredSegments().contains(segment)) {
                    skip = true;
                    break;
                }
            }
            if (!skip)
                files.append({relative, QString(), it.fileInfo().size()});
            if (files.size() > 400000 || (cancel && cancel()))
                break;
        }
    }
    if (total)
        *total = int(files.size());
    return files;
}

QHash<QString, QByteArray> readBlobs(const QString &root, const QList<Listed> &entries,
                                     const CancelFn &cancel)
{
    QHash<QString, QByteArray> blobs;
    if (entries.isEmpty())
        return blobs;
    QByteArray input;
    for (const Listed &entry : entries)
        input += entry.object.toLatin1() + '\n';
    bool ok = false;
    const QByteArray out = runGit(root, {QStringLiteral("cat-file"), QStringLiteral("--batch")}, &ok,
                                  300000, cancel, input);
    qsizetype at = 0;
    for (const Listed &entry : entries) {
        const qsizetype lineEnd = out.indexOf('\n', at);
        if (lineEnd < 0)
            break;
        const QList<QByteArray> header = out.mid(at, lineEnd - at).split(' ');
        at = lineEnd + 1;
        if (header.size() < 3) {
            continue;
        }
        const qsizetype size = header.at(2).toLongLong();
        blobs.insert(entry.path, out.mid(at, size));
        at += size + 1;
    }
    return blobs;
}

QByteArray readWorkingFile(const QString &root, const QString &path, qint64 limit)
{
    QFile file(QDir(root).filePath(path));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    if (file.size() > limit)
        return {};
    return file.readAll();
}

void readPackageRoots(Snapshot &snapshot, const QList<Listed> &all, bool bare, const CancelFn &cancel)
{
    QList<Listed> manifests;
    for (const Listed &entry : all) {
        const QString name = fileNameOf(entry.path);
        if ((name == QLatin1String("go.mod") || name == QLatin1String("pubspec.yaml")) &&
            !isVendoredPath(entry.path))
            manifests.append(entry);
        if (manifests.size() > 200)
            break;
    }
    QHash<QString, QByteArray> contents;
    if (bare)
        contents = readBlobs(snapshot.root, manifests, cancel);
    else
        for (const Listed &entry : std::as_const(manifests))
            contents.insert(entry.path, readWorkingFile(snapshot.root, entry.path, 1 << 20));
    thread_local const QRegularExpression goModule(QStringLiteral("^\\s*module\\s+(\\S+)"),
                                             QRegularExpression::MultilineOption);
    thread_local const QRegularExpression dartName(QStringLiteral("^name:\\s*([A-Za-z0-9_]+)"),
                                             QRegularExpression::MultilineOption);
    for (auto it = contents.cbegin(); it != contents.cend(); ++it) {
        const QString text = QString::fromUtf8(it.value());
        const QString dir = dirOf(it.key());
        if (it.key().endsWith(QLatin1String("go.mod"))) {
            const QRegularExpressionMatch m = goModule.match(text);
            if (m.hasMatch())
                snapshot.packageRoots.insert(QStringLiteral("go:") + m.captured(1).remove(QLatin1Char('"')), dir);
        } else {
            const QRegularExpressionMatch m = dartName.match(text);
            if (m.hasMatch())
                snapshot.packageRoots.insert(QStringLiteral("dart:") + m.captured(1), dir);
        }
    }
}

}

SnapshotPtr analyze(const QString &rootPath, const Options &options, const ProgressFn &progress,
                    const CancelFn &cancel)
{
    QElapsedTimer timer;
    timer.start();
    auto snapshot = std::make_shared<Snapshot>();
    const QString root = QDir::cleanPath(QFileInfo(rootPath).absoluteFilePath());
    snapshot->root = root;
    QString name = QFileInfo(root).fileName();
    if (name.endsWith(QLatin1String(".git")))
        name.chop(4);
    snapshot->name = name;
    snapshot->analyzedAt = QDateTime::currentSecsSinceEpoch();
    const auto report = [&progress](const QString &text, double fraction) {
        if (progress)
            progress(text, clamp01(fraction));
    };
    const auto canceled = [&cancel] { return cancel && cancel(); };
    if (!QFileInfo(root).isDir()) {
        snapshot->error = QStringLiteral("The folder %1 does not exist on this computer.").arg(root);
        return snapshot;
    }
    report(QStringLiteral("Listing files in %1…").arg(name), 0.01);
    const bool bare = gitText(root, {QStringLiteral("rev-parse"), QStringLiteral("--is-bare-repository")}) ==
                      QLatin1String("true");
    const bool git = bare || gitText(root, {QStringLiteral("rev-parse"),
                                            QStringLiteral("--is-inside-work-tree")}) == QLatin1String("true");
    snapshot->git = git;
    if (git) {
        snapshot->head = gitText(root, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
        snapshot->branch = gitText(root, {QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"),
                                          QStringLiteral("HEAD")});
    }
    int listedTotal = 0;
    QList<Listed> all = listFiles(root, git, bare, &listedTotal, cancel);
    if (git && !bare) {
        bool anySource = false;
        for (const Listed &entry : std::as_const(all)) {
            if (!languageForPath(entry.path).isEmpty()) {
                anySource = true;
                break;
            }
        }
        if (!anySource) {
            all = listFiles(root, false, false, &listedTotal, cancel);
            snapshot->git = false;
        }
    }
    const bool history = snapshot->git;
    if (canceled())
        return {};
    QList<Listed> sources;
    int skipped = 0;
    for (const Listed &entry : all) {
        if (languageForPath(entry.path).isEmpty())
            continue;
        if (isVendoredPath(entry.path) || (entry.size > options.maxFileBytes)) {
            ++skipped;
            continue;
        }
        sources.append(entry);
    }
    std::sort(sources.begin(), sources.end(), [](const Listed &a, const Listed &b) { return a.path < b.path; });
    const int sourceTotal = int(sources.size());
    if (sources.size() > options.maxFiles) {
        sources = sources.mid(0, options.maxFiles);
        snapshot->warning = QStringLiteral("Analyzed the first %1 of %2 source files")
                                .arg(QLocale().toString(options.maxFiles), QLocale().toString(sourceTotal));
    }
    readPackageRoots(*snapshot, all, bare, cancel);
    if (sources.isEmpty()) {
        snapshot->error = QStringLiteral("No source code found in %1.").arg(name);
        snapshot->elapsedMs = timer.elapsed();
        return snapshot;
    }
    static QThreadPool *measurePool = [] {
        auto *pool = new QThreadPool;
        pool->setMaxThreadCount(qMax(2, QThread::idealThreadCount() - 1));
        pool->setObjectName(QStringLiteral("RepoInsightMeasure"));
        return pool;
    }();
    QVector<QVector<int>> significant;
    QVector<QVector<quint64>> windows;
    const int chunkSize = 1200;
    const qint64 limit = options.maxFileBytes;
    for (int start = 0; start < sources.size(); start += chunkSize) {
        if (canceled())
            return {};
        const QList<Listed> chunk = sources.mid(start, chunkSize);
        report(QStringLiteral("Reading %1 of %2 files…")
                   .arg(QLocale().toString(qMin<int>(start + chunkSize, int(sources.size()))),
                        QLocale().toString(sources.size())),
               0.04 + 0.6 * double(start) / double(sources.size()));
        QHash<QString, QByteArray> blobs;
        if (bare)
            blobs = readBlobs(root, chunk, cancel);
        const std::function<Measured(const Listed &)> measure = [&](const Listed &entry) -> Measured {
            const QByteArray bytes = bare ? blobs.value(entry.path) : readWorkingFile(root, entry.path, limit);
            Measured measured;
            if (bytes.isEmpty() || binaryText(bytes) || generatedText(bytes) ||
                (bytes.size() > 20000 && bytes.count('\n') < bytes.size() / 400)) {
                measured.skip = true;
                measured.info.path = entry.path;
                return measured;
            }
            return measureInternal(entry.path, bytes, true);
        };
        const QList<Measured> results = QtConcurrent::blockingMapped<QList<Measured>>(measurePool, chunk, measure);
        for (const Measured &measured : results) {
            if (measured.skip) {
                ++skipped;
                continue;
            }
            snapshot->byPath.insert(measured.info.path, int(snapshot->files.size()));
            snapshot->files.append(measured.info);
            significant.append(measured.significant);
            windows.append(measured.windows);
        }
    }
    snapshot->skippedFiles = skipped;
    int cpp = 0;
    int c = 0;
    int objc = 0;
    for (const FileInfo &file : std::as_const(snapshot->files)) {
        if (file.language == QLatin1String("C++"))
            ++cpp;
        else if (file.language == QLatin1String("C") && !file.path.endsWith(QLatin1String(".h")))
            ++c;
        else if (file.language == QLatin1String("Objective-C"))
            ++objc;
    }
    const QString headerLanguage = objc > cpp && objc > c ? QStringLiteral("Objective-C")
                                   : cpp >= c && cpp > 0  ? QStringLiteral("C++")
                                                          : QString();
    if (!headerLanguage.isEmpty())
        for (FileInfo &file : snapshot->files)
            if (file.path.endsWith(QLatin1String(".h")) && file.language == QLatin1String("C"))
                file.language = headerLanguage;
    if (canceled())
        return {};
    report(QStringLiteral("Connecting imports…"), 0.66);
    resolveDependencies(*snapshot);
    markCycles(*snapshot);
    report(QStringLiteral("Looking for duplicated code…"), 0.7);
    applyWindows(*snapshot, significant, windows);
    significant.clear();
    windows.clear();
    if (history && options.history) {
        if (canceled())
            return {};
        report(QStringLiteral("Reading git history…"), 0.74);
        bool ok = false;
        const QByteArray log = runGit(
            root,
            {QStringLiteral("log"), QStringLiteral("--no-merges"), QStringLiteral("--no-renames"),
             QStringLiteral("--relative"), QStringLiteral("--numstat"),
             QStringLiteral("--since=%1.days").arg(options.historyDays),
             QStringLiteral("-n"), QString::number(options.maxCommits),
             QStringLiteral("--format=%x1e%H%x1f%an%x1f%at%x1f%s")},
            &ok, 300000, cancel);
        if (canceled())
            return {};
        report(QStringLiteral("Finding files that change together…"), 0.9);
        applyHistory(*snapshot, log, options, QDateTime::currentSecsSinceEpoch());
    }
    report(QStringLiteral("Scoring code health…"), 0.95);
    for (FileInfo &file : snapshot->files) {
        QStringList issues;
        file.health = computeHealth(file, &issues);
        file.healthIssues = issues;
    }
    computeHotspots(*snapshot);
    snapshot->elapsedMs = timer.elapsed();
    report(QStringLiteral("Done"), 1.0);
    return snapshot;
}

QList<TrendPoint> complexityTrend(const QString &root, const QString &path, int samples,
                                  const CancelFn &cancel)
{
    QList<TrendPoint> points;
    bool ok = false;
    const QByteArray log = runGit(root, {QStringLiteral("log"), QStringLiteral("--no-merges"),
                                         QStringLiteral("-n"), QStringLiteral("400"),
                                         QStringLiteral("--format=%H%x1f%at%x1f%an"),
                                         QStringLiteral("--"), path},
                                  &ok, 60000, cancel);
    if (!ok)
        return points;
    QList<QList<QByteArray>> commits;
    for (const QByteArray &line : log.split('\n')) {
        const QList<QByteArray> fields = line.split('\x1f');
        if (fields.size() >= 3)
            commits.append(fields);
    }
    if (commits.isEmpty())
        return points;
    const int want = qMax(2, samples);
    QVector<int> picks;
    if (commits.size() <= want) {
        for (int i = 0; i < commits.size(); ++i)
            picks.append(i);
    } else {
        for (int k = 0; k < want; ++k) {
            const int index = int(std::lround(double(k) * double(commits.size() - 1) / double(want - 1)));
            if (picks.isEmpty() || picks.last() != index)
                picks.append(index);
        }
    }
    for (auto it = picks.crbegin(); it != picks.crend(); ++it) {
        if (cancel && cancel())
            return {};
        const QList<QByteArray> &fields = commits.at(*it);
        const QString sha = QString::fromLatin1(fields.at(0));
        bool shown = false;
        const QByteArray bytes = runGit(root, {QStringLiteral("show"), sha + QStringLiteral(":./") + path},
                                        &shown, 30000, cancel);
        if (!shown)
            continue;
        FileInfo info = measureSource(path, bytes);
        TrendPoint point;
        point.commit = sha;
        point.time = fields.at(1).toLongLong();
        point.author = QString::fromUtf8(fields.at(2));
        point.codeLines = info.codeLines;
        point.complexity = info.complexity;
        point.maxComplexity = info.maxComplexity;
        point.health = computeHealth(info, nullptr);
        points.append(point);
    }
    return points;
}

namespace {

struct CacheEntry {
    QFuture<SnapshotPtr> future;
    SnapshotPtr snapshot;
    std::shared_ptr<std::atomic_bool> canceled;
    qint64 finishedAt = 0;
};

QMutex &cacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, CacheEntry> &cache()
{
    static QHash<QString, CacheEntry> entries;
    return entries;
}

QThreadPool *analysisPool()
{
    static QThreadPool *pool = [] {
        auto *created = new QThreadPool;
        created->setMaxThreadCount(2);
        created->setObjectName(QStringLiteral("RepoInsightAnalysis"));
        return created;
    }();
    return pool;
}

QString cacheKey(const QString &root)
{
    return QDir::cleanPath(QFileInfo(root).absoluteFilePath());
}

QFuture<SnapshotPtr> readyFuture(const SnapshotPtr &snapshot)
{
    QPromise<SnapshotPtr> promise;
    QFuture<SnapshotPtr> future = promise.future();
    promise.start();
    promise.setProgressRange(0, 1000);
    promise.setProgressValue(1000);
    promise.addResult(snapshot);
    promise.finish();
    return future;
}

constexpr qint64 kFreshSeconds = 600;

}

QFuture<SnapshotPtr> Analysis::request(const QString &root, bool force)
{
    const QString key = cacheKey(root);
    QMutexLocker locker(&cacheMutex());
    CacheEntry &entry = cache()[key];
    if (entry.future.isValid() && entry.future.isRunning() && !force)
        return entry.future;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!force && entry.snapshot && now - entry.finishedAt < kFreshSeconds)
        return readyFuture(entry.snapshot);
    if (entry.canceled)
        entry.canceled->store(true);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    entry.canceled = canceled;
    entry.future = QtConcurrent::run(analysisPool(), [key, canceled](QPromise<SnapshotPtr> &promise) {
        promise.setProgressRange(0, 1000);
        const ProgressFn progress = [&promise](const QString &text, double fraction) {
            promise.setProgressValueAndText(int(fraction * 1000), text);
        };
        const CancelFn cancel = [&promise, canceled] { return canceled->load() || promise.isCanceled(); };
        SnapshotPtr snapshot = analyze(key, Options(), progress, cancel);
        if (!snapshot || cancel())
            return;
        {
            QMutexLocker inner(&cacheMutex());
            CacheEntry &stored = cache()[key];
            if (stored.canceled == canceled) {
                stored.snapshot = snapshot;
                stored.finishedAt = QDateTime::currentSecsSinceEpoch();
            }
        }
        promise.addResult(snapshot);
    });
    return entry.future;
}

SnapshotPtr Analysis::cached(const QString &root)
{
    QMutexLocker locker(&cacheMutex());
    const auto it = cache().constFind(cacheKey(root));
    return it == cache().cend() ? SnapshotPtr() : it->snapshot;
}

bool Analysis::running(const QString &root)
{
    QMutexLocker locker(&cacheMutex());
    const auto it = cache().constFind(cacheKey(root));
    return it != cache().cend() && it->future.isValid() && it->future.isRunning();
}

bool Analysis::fresh(const QString &root)
{
    QMutexLocker locker(&cacheMutex());
    const auto it = cache().constFind(cacheKey(root));
    return it != cache().cend() && it->snapshot &&
           QDateTime::currentSecsSinceEpoch() - it->finishedAt < kFreshSeconds;
}

void Analysis::cancel(const QString &root)
{
    QMutexLocker locker(&cacheMutex());
    const auto it = cache().find(cacheKey(root));
    if (it != cache().end() && it->canceled)
        it->canceled->store(true);
}

} // namespace forkmesh::insight
