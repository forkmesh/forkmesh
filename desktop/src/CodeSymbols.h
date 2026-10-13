#pragma once

// Code-symbol heuristics for the Files editor's navigation (adhoc #1607): what
// a source line defines, whether it calls a given symbol, which function a line
// sits inside, and what a function body calls. Together these answer the two
// questions the editor's right-click menu asks — "where else does this appear"
// and "trace this function backwards (who calls it) and forwards (what it
// calls)" — without a language server.
//
// The rules are deliberately language-agnostic: one set of shapes that covers
// C/C++/Java/C#/JS/TS/Go/Rust/Python/Ruby/shell well enough to navigate. They
// are heuristics, not a parser, so they are kept here — pure, GUI-free and unit
// tested (tests/test_crypto.cpp) — rather than buried in the window code that
// runs `git grep` over them (MainWindowFiles.cpp).

#include <QChar>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace forkmesh::code {

// The four questions the editor can ask about a symbol. Definitions and
// references are flat lookups; the two call directions are the trace — Callers
// walks backwards through the code that reaches this function, Calls walks
// forwards through what it reaches.
enum TraceMode {
    TraceDefinitions = 0,
    TraceReferences,
    TraceCallers,
    TraceCalls,
    TraceModeCount,
};

// `$` counts: it is an ordinary identifier character in JS/PHP, and treating it
// as a separator would split jQuery-style names in half.
inline bool isIdentifierChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
}

// Words that start a statement rather than a definition. A line whose leading
// word is one of these is never a signature, however much it looks like one
// ("if (ready) {", "return build(x)").
inline bool isStatementKeyword(const QString &word)
{
    static const QSet<QString> words = {
        QStringLiteral("if"),      QStringLiteral("else"),
        QStringLiteral("for"),     QStringLiteral("foreach"),
        QStringLiteral("while"),   QStringLiteral("do"),
        QStringLiteral("switch"),  QStringLiteral("case"),
        QStringLiteral("catch"),   QStringLiteral("try"),
        QStringLiteral("finally"), QStringLiteral("return"),
        QStringLiteral("throw"),   QStringLiteral("raise"),
        QStringLiteral("yield"),   QStringLiteral("await"),
        QStringLiteral("emit"),    QStringLiteral("new"),
        QStringLiteral("delete"),  QStringLiteral("sizeof"),
        QStringLiteral("assert"),  QStringLiteral("with"),
        QStringLiteral("elif"),    QStringLiteral("elsif"),
        QStringLiteral("unless"),  QStringLiteral("until"),
        QStringLiteral("import"),  QStringLiteral("include"),
        QStringLiteral("require"), QStringLiteral("using"),
        QStringLiteral("go"),      QStringLiteral("defer"),
        QStringLiteral("in"),      QStringLiteral("not"),
        QStringLiteral("and"),     QStringLiteral("or"),
        QStringLiteral("co_return"),
    };
    return words.contains(word);
}

// Statement keywords plus the type/declaration words and literals no language
// would call. Used to keep "what does this function call" from listing `if`,
// `int` and `true` as callees.
inline bool isLanguageKeyword(const QString &word)
{
    static const QSet<QString> words = {
        QStringLiteral("class"),    QStringLiteral("struct"),
        QStringLiteral("enum"),     QStringLiteral("union"),
        QStringLiteral("typedef"),  QStringLiteral("template"),
        QStringLiteral("typename"), QStringLiteral("namespace"),
        QStringLiteral("operator"), QStringLiteral("static"),
        QStringLiteral("const"),    QStringLiteral("constexpr"),
        QStringLiteral("inline"),   QStringLiteral("virtual"),
        QStringLiteral("override"), QStringLiteral("final"),
        QStringLiteral("public"),   QStringLiteral("private"),
        QStringLiteral("protected"),QStringLiteral("friend"),
        QStringLiteral("extern"),   QStringLiteral("auto"),
        QStringLiteral("void"),     QStringLiteral("bool"),
        QStringLiteral("char"),     QStringLiteral("int"),
        QStringLiteral("long"),     QStringLiteral("short"),
        QStringLiteral("float"),    QStringLiteral("double"),
        QStringLiteral("unsigned"), QStringLiteral("signed"),
        QStringLiteral("true"),     QStringLiteral("false"),
        QStringLiteral("null"),     QStringLiteral("nullptr"),
        QStringLiteral("none"),     QStringLiteral("nil"),
        QStringLiteral("this"),     QStringLiteral("self"),
        QStringLiteral("super"),    QStringLiteral("def"),
        QStringLiteral("function"), QStringLiteral("func"),
        QStringLiteral("fn"),       QStringLiteral("lambda"),
        QStringLiteral("var"),      QStringLiteral("let"),
        QStringLiteral("val"),      QStringLiteral("async"),
        QStringLiteral("export"),   QStringLiteral("default"),
        QStringLiteral("interface"),QStringLiteral("trait"),
        QStringLiteral("impl"),     QStringLiteral("module"),
        QStringLiteral("package"),  QStringLiteral("type"),
    };
    return isStatementKeyword(word) || words.contains(word);
}

// The identifier `column` sits in, or "" when the caret is on whitespace or
// punctuation. A caret immediately *after* a word still means that word, which
// is where a click at the end of a name leaves it.
inline QString identifierAt(const QString &line, int column)
{
    if (line.isEmpty())
        return {};
    int pos = qBound(0, column, line.size());
    if (pos == line.size() || !isIdentifierChar(line.at(pos))) {
        if (pos == 0 || !isIdentifierChar(line.at(pos - 1)))
            return {};
        --pos;
    }
    int start = pos;
    int end = pos;
    while (start > 0 && isIdentifierChar(line.at(start - 1)))
        --start;
    while (end + 1 < line.size() && isIdentifierChar(line.at(end + 1)))
        ++end;
    const QString word = line.mid(start, end - start + 1);
    // A bare number (or the numeric tail of one) is not a symbol.
    return word.isEmpty() || word.at(0).isDigit() ? QString() : word;
}

// Leading whitespace as a column count, a tab counting as four columns — the
// measure the enclosing-definition walk compares nesting with.
inline int indentWidth(const QString &line)
{
    int width = 0;
    for (const QChar c : line) {
        if (c == QLatin1Char('\t'))
            width += 4;
        else if (c == QLatin1Char(' '))
            width += 1;
        else
            break;
    }
    return width;
}

// Drop string literals and trailing line comments so a call scan doesn't match
// text inside a message. Crude by design: one line at a time, no block-comment
// state, because the callers only ever look at single lines.
inline QString stripCodeNoise(const QString &line)
{
    QString out;
    out.reserve(line.size());
    QChar quote;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\'))
                ++i; // skip the escaped character
            else if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            quote = c;
            out += QLatin1Char(' ');
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < line.size() &&
            line.at(i + 1) == QLatin1Char('/'))
            break;
        if (c == QLatin1Char('#'))
            break;
        out += c;
    }
    return out;
}

// The symbol this line defines, or "" when it defines nothing. Five shapes, in
// the order they are cheapest to rule out.
inline QString definitionName(const QString &rawLine)
{
    const QString line = rawLine.trimmed();
    if (line.isEmpty())
        return {};

    // A preprocessor macro is a definition; every other '#' line is a comment
    // or an include, and a '//' or '*' line is a comment.
    if (line.startsWith(QLatin1Char('#'))) {
        static const QRegularExpression define(
            QStringLiteral("^#\\s*define\\s+([A-Za-z_][A-Za-z0-9_]*)"));
        const QRegularExpressionMatch m = define.match(line);
        return m.hasMatch() ? m.captured(1) : QString();
    }
    if (line.startsWith(QLatin1String("//")) ||
        line.startsWith(QLatin1String("/*")) || line.startsWith(QLatin1Char('*')))
        return {};

    // 1. "<modifiers> <keyword> <name>" — the languages that spell a definition
    //    with a leading keyword (def, function, class, struct, fn, func, …).
    static const QRegularExpression keyword(QStringLiteral(
        "^(?:(?:export|public|private|protected|internal|static|final|abstract|"
        "async|inline|extern|virtual|open|declare|local|readonly|const|"
        "pub(?:\\([a-z:]+\\))?)\\s+)*"
        "(?:def|function|fn|func|sub|proc|procedure|class|struct|interface|enum|"
        "trait|impl|namespace|module|type|record|union|protocol|object)\\s+"
        "([A-Za-z_$][A-Za-z0-9_$]*)"));
    QRegularExpressionMatch m = keyword.match(line);
    if (m.hasMatch())
        return m.captured(1);

    // 2. A Go method, whose receiver sits between the keyword and the name.
    static const QRegularExpression goMethod(QStringLiteral(
        "^func\\s*\\([^)]*\\)\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*\\("));
    m = goMethod.match(line);
    if (m.hasMatch())
        return m.captured(1);

    // 3. A function bound to a name: "const handler = (e) => …",
    //    "onClick: function (…)", "run = async () => …".
    static const QRegularExpression assigned(QStringLiteral(
        "^(?:(?:export|static|final|public|private|protected)\\s+)*"
        "(?:(?:const|let|var|val)\\s+)?([A-Za-z_$][A-Za-z0-9_$]*)\\s*"
        "(?::[^=]*)?[:=]\\s*(?:async\\s+)?"
        "(?:function\\b|\\([^()]*\\)\\s*(?::[^=]+)?=>|[A-Za-z_$][A-Za-z0-9_$]*\\s*=>)"));
    m = assigned.match(line);
    if (m.hasMatch())
        return m.captured(1);

    // 4. A brace-language signature: the last identifier before the first "(",
    //    with a return type / receiver in front of it and no statement
    //    punctuation after it.
    const int paren = line.indexOf(QLatin1Char('('));
    if (paren <= 0)
        return {};
    const QString head = line.left(paren);
    int end = head.size();
    while (end > 0 && head.at(end - 1).isSpace())
        --end;
    int start = end;
    while (start > 0 && isIdentifierChar(head.at(start - 1)))
        --start;
    const QString name = head.mid(start, end - start);
    if (name.isEmpty() || name.at(0).isDigit() || isLanguageKeyword(name))
        return {};
    const QString prefix = head.left(start).trimmed();
    // The prefix may only be type-ish: identifiers, scope/template punctuation
    // and pointer marks. A dot, an arrow or an operator means a statement.
    static const QRegularExpression typePrefix(
        QStringLiteral("^[A-Za-z0-9_:<>,&*~\\s\\[\\]]*$"));
    if (!typePrefix.match(prefix).hasMatch())
        return {};
    static const QRegularExpression firstWord(
        QStringLiteral("^([A-Za-z_][A-Za-z0-9_]*)"));
    const QRegularExpressionMatch lead = firstWord.match(prefix);
    if (lead.hasMatch() && isStatementKeyword(lead.captured(1)))
        return {};
    if (line.mid(paren).contains(QLatin1Char(';')))
        return {}; // a call or a forward declaration, not a body
    // Read on its own, the continuation line of a wrapped expression has the
    // exact shape of a signature: "ref, QStringLiteral(" is an argument list and
    // ": formatByteSize(" is the far half of a ternary. Neither can be a return
    // type, which never ends in a comma nor in a lone colon.
    if (prefix.endsWith(QLatin1Char(',')) ||
        (prefix.endsWith(QLatin1Char(':')) && !prefix.endsWith(QLatin1String("::"))))
        return {};
    if (!prefix.isEmpty()) {
        // A prefix that is *only* a scope chain — "QString::" from a wrapped
        // "QString::fromUtf8(…)" — has no return type in front of it, so the
        // only definition it can spell is a constructor, whose name repeats the
        // scope it sits in ("Foo::Foo("). Anything else is a static call being
        // continued from the line above. ("void", a one-word return type, must
        // not read as a scope chain: it does not end in "::". A destructor's
        // "Foo::~" is not a scope chain either, so it never reaches here.)
        static const QRegularExpression scopeChainOnly(QStringLiteral(
            "^([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)::$"));
        const QRegularExpressionMatch chain = scopeChainOnly.match(prefix);
        if (chain.hasMatch() && !chain.captured(1).endsWith(QLatin1String("::") + name) &&
            chain.captured(1) != name)
            return {};
        return name;
    }
    // No return type at all ("render() {"): only a brace-opening line with its
    // own argument list balanced, so a trailing-lambda call like
    // "connect(a, b, [this] {" is not mistaken for a definition.
    if (!line.endsWith(QLatin1Char('{')))
        return {};
    return line.count(QLatin1Char('(')) == line.count(QLatin1Char(')')) ? name
                                                                        : QString();
}

inline bool definesSymbol(const QString &line, const QString &symbol)
{
    return !symbol.isEmpty() && definitionName(line) == symbol;
}

// A call of `symbol` on this line — the name followed by an opening paren.
inline bool callsSymbol(const QString &line, const QString &symbol)
{
    if (symbol.isEmpty())
        return false;
    const QRegularExpression call(QStringLiteral("\\b") +
                                  QRegularExpression::escape(symbol) +
                                  QStringLiteral("\\s*\\("));
    return call.match(line).hasMatch() && !definesSymbol(line, symbol);
}

// The definition a line sits inside: walk up, skipping anything nested deeper,
// until a definition at a shallower indent turns up. `line` is 1-based.
struct Enclosing {
    QString name;
    int line = 0; // 1-based line of the definition, 0 when there is none
};

inline Enclosing enclosingDefinition(const QStringList &lines, int line)
{
    if (line < 1 || line > lines.size())
        return {};
    const QString own = definitionName(lines.at(line - 1));
    if (!own.isEmpty())
        return {own, line};
    int limit = indentWidth(lines.at(line - 1));
    if (limit == 0)
        return {}; // a statement at column 0 belongs to the file, not a function
    const int stop = qMax(1, line - 4000);
    // Once the walk reaches column 0 the signature is at most a few lines away
    // (an Allman brace, a multi-line return type); past that we are in the
    // previous definition and should stop rather than name it.
    int columnZeroBudget = 40;
    for (int i = line - 1; i >= stop; --i) {
        const QString &text = lines.at(i - 1);
        if (text.trimmed().isEmpty())
            continue;
        const int indent = indentWidth(text);
        if (indent > limit)
            continue;
        const QString name = definitionName(text);
        if (!name.isEmpty())
            return {name, i};
        limit = indent;
        if (limit == 0 && --columnZeroBudget <= 0)
            return {};
    }
    return {};
}

// The last line of the definition that starts at `defLine` (1-based). Brace
// languages are followed by their braces; everything else by indentation.
inline int definitionBodyEnd(const QStringList &lines, int defLine)
{
    if (defLine < 1 || defLine > lines.size())
        return 0;
    const QString def = lines.at(defLine - 1);
    const int defIndent = indentWidth(def);
    const int limit = qMin(lines.size(), defLine + 4000);

    bool braces = false;
    if (!def.trimmed().endsWith(QLatin1Char(':'))) {
        if (stripCodeNoise(def).contains(QLatin1Char('{'))) {
            braces = true;
        } else {
            for (int i = defLine + 1; i <= limit; ++i) {
                const QString next = lines.at(i - 1).trimmed();
                if (next.isEmpty())
                    continue;
                braces = next.startsWith(QLatin1Char('{'));
                break;
            }
        }
    }

    if (braces) {
        int depth = 0;
        bool opened = false;
        for (int i = defLine; i <= limit; ++i) {
            for (const QChar c : stripCodeNoise(lines.at(i - 1))) {
                if (c == QLatin1Char('{')) {
                    ++depth;
                    opened = true;
                } else if (c == QLatin1Char('}')) {
                    --depth;
                    if (opened && depth <= 0)
                        return i;
                }
            }
        }
        return limit;
    }

    int end = defLine;
    for (int i = defLine + 1; i <= limit; ++i) {
        const QString &text = lines.at(i - 1);
        if (text.trimmed().isEmpty())
            continue;
        if (indentWidth(text) <= defIndent)
            return end;
        end = i;
    }
    return end;
}

// Every distinct symbol the definition at `defLine` calls, in source order,
// with the line of its first call — the "trace this forwards" list.
struct CallSite {
    QString name;
    int line = 0;
};

inline QVector<CallSite> callsInBody(const QStringList &lines, int defLine,
                                     int maxCalls = 200)
{
    QVector<CallSite> calls;
    if (defLine < 1 || defLine > lines.size())
        return calls;
    const QString owner = definitionName(lines.at(defLine - 1));
    const int end = definitionBodyEnd(lines, defLine);
    static const QRegularExpression call(
        QStringLiteral("\\b([A-Za-z_$][A-Za-z0-9_$]*)\\s*\\("));
    QSet<QString> seen;
    for (int i = defLine; i <= end && i <= lines.size(); ++i) {
        QRegularExpressionMatchIterator it =
            call.globalMatch(stripCodeNoise(lines.at(i - 1)));
        while (it.hasNext()) {
            const QString name = it.next().captured(1);
            if (name.size() < 2 || name == owner || isLanguageKeyword(name))
                continue;
            if (seen.contains(name))
                continue;
            seen.insert(name);
            calls.append({name, i});
            if (calls.size() >= maxCalls)
                return calls;
        }
    }
    return calls;
}

} // namespace forkmesh::code
