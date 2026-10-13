#include "PromptSpellCheckHighlighter.h"

#include <QAction>
#include <QColor>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextCharFormat>

#include <algorithm>
#include <utility>

namespace {

constexpr int kMaxSuggestions = 6;

QString normalizedWord(QString word)
{
    word.replace(QChar(0x2019), QLatin1Char('\''));
    return word.toCaseFolded();
}

// Optimal string alignment distance, giving up once it must exceed `limit`.
int editDistance(const QString &a, const QString &b, int limit)
{
    if (qAbs(a.size() - b.size()) > limit)
        return limit + 1;
    const int n = int(a.size());
    const int m = int(b.size());
    QList<int> previous2(m + 1), previous(m + 1), current(m + 1);
    for (int j = 0; j <= m; ++j)
        previous[j] = j;
    for (int i = 1; i <= n; ++i) {
        current[0] = i;
        int rowBest = current[0];
        for (int j = 1; j <= m; ++j) {
            const int cost = a.at(i - 1) == b.at(j - 1) ? 0 : 1;
            int value = std::min({previous[j] + 1, current[j - 1] + 1,
                                  previous[j - 1] + cost});
            if (i > 1 && j > 1 && a.at(i - 1) == b.at(j - 2) &&
                a.at(i - 2) == b.at(j - 1))
                value = std::min(value, previous2[j - 2] + 1);
            current[j] = value;
            rowBest = std::min(rowBest, value);
        }
        if (rowBest > limit)
            return limit + 1;
        std::swap(previous2, previous);
        std::swap(previous, current);
    }
    return previous[m];
}

// Nearest words by edit distance, for dictionaries without a suggest engine.
QStringList nearestWords(const QString &word, const QSet<QString> &words)
{
    const QString needle = normalizedWord(word);
    const int limit = needle.size() <= 3 ? 1 : 2;
    QList<QPair<int, QString>> ranked;
    for (const QString &candidate : words) {
        const int distance = editDistance(needle, candidate, limit);
        if (distance <= limit)
            ranked.append({distance, candidate});
    }
    std::sort(ranked.begin(), ranked.end(), [&needle](const auto &a, const auto &b) {
        if (a.first != b.first)
            return a.first < b.first;
        const bool aSameStart = a.second.startsWith(needle.front());
        const bool bSameStart = b.second.startsWith(needle.front());
        if (aSameStart != bSameStart)
            return aSameStart;
        return a.second < b.second;
    });

    const bool capitalized = word.front().isUpper();
    QStringList result;
    for (const auto &entry : ranked) {
        QString suggestion = entry.second;
        if (capitalized)
            suggestion.front() = suggestion.front().toUpper();
        result.append(suggestion);
        if (result.size() == kMaxSuggestions)
            break;
    }
    return result;
}

QStringList dictionaryLocaleNames()
{
    QString locale = QLocale::system().name();
    if (locale.isEmpty() || locale == QLatin1String("C"))
        locale = QStringLiteral("en_US");
    locale.replace(QLatin1Char('-'), QLatin1Char('_'));

    QStringList names{locale};
    const QString language = locale.section(QLatin1Char('_'), 0, 0);
    if (!language.isEmpty() && !names.contains(language))
        names.append(language);
    // The composer is English-first today. An English dictionary is more
    // useful than no spell checking when the OS locale has no matching files.
    if (!names.contains(QStringLiteral("en_US")))
        names.append(QStringLiteral("en_US"));
    if (!names.contains(QStringLiteral("en_GB")))
        names.append(QStringLiteral("en_GB"));
    return names;
}

QStringList dictionaryRoots()
{
    QStringList roots;
    for (const QString &dataRoot :
         QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        roots << QDir(dataRoot).filePath(QStringLiteral("hunspell"))
              << QDir(dataRoot).filePath(QStringLiteral("myspell/dicts"));
    }
    roots << QStringLiteral("/usr/share/hunspell")
          << QStringLiteral("/usr/share/myspell/dicts")
          << QStringLiteral("/usr/local/share/hunspell");
    roots.removeDuplicates();
    return roots;
}

std::pair<QString, QString> hunspellFiles()
{
    const QStringList locales = dictionaryLocaleNames();
    for (const QString &root : dictionaryRoots()) {
        for (const QString &locale : locales) {
            const QStringList spellings = {
                locale, QString(locale).replace(QLatin1Char('_'), QLatin1Char('-'))};
            for (const QString &spelling : spellings) {
                const QString stem = QDir(root).filePath(spelling);
                const QString aff = stem + QStringLiteral(".aff");
                const QString dic = stem + QStringLiteral(".dic");
                if (QFileInfo(aff).isReadable() && QFileInfo(dic).isReadable())
                    return {aff, dic};
            }
        }
    }
    return {};
}

class SystemDictionary
{
public:
    SystemDictionary()
    {
        const auto [affPath, dicPath] = hunspellFiles();
        if (!affPath.isEmpty()) {
            const QStringList libraryNames = {
                QStringLiteral("hunspell-1.7"),
                QStringLiteral("hunspell-1.6"),
                QStringLiteral("libhunspell-1.7.so.0"),
                QStringLiteral("libhunspell-1.6.so.0"),
                QStringLiteral("libhunspell-1.7.dylib"),
                QStringLiteral("libhunspell.dylib"),
                QStringLiteral("hunspell.dll"),
                QStringLiteral("libhunspell.dll")};
            for (const QString &name : libraryNames) {
                m_library.setFileName(name);
                if (!m_library.load())
                    continue;
                m_create = reinterpret_cast<CreateFunction>(
                    m_library.resolve("Hunspell_create"));
                m_destroy = reinterpret_cast<DestroyFunction>(
                    m_library.resolve("Hunspell_destroy"));
                m_spell = reinterpret_cast<SpellFunction>(
                    m_library.resolve("Hunspell_spell"));
                m_suggest = reinterpret_cast<SuggestFunction>(
                    m_library.resolve("Hunspell_suggest"));
                m_freeList = reinterpret_cast<FreeListFunction>(
                    m_library.resolve("Hunspell_free_list"));
                if (m_create && m_destroy && m_spell) {
                    const QByteArray aff = QFile::encodeName(affPath);
                    const QByteArray dic = QFile::encodeName(dicPath);
                    m_handle = m_create(aff.constData(), dic.constData());
                }
                if (m_handle)
                    return;
                m_library.unload();
            }
        }

        loadPlainWordList();
    }

    ~SystemDictionary()
    {
        if (m_handle && m_destroy)
            m_destroy(m_handle);
    }

    bool available() const
    {
        return m_handle || !m_words.isEmpty();
    }

    bool accepts(const QString &word) const
    {
        if (m_handle)
            return m_spell(m_handle, word.toUtf8().constData()) != 0;
        return m_words.contains(normalizedWord(word));
    }

    QStringList suggestions(const QString &word) const
    {
        if (!m_handle)
            return nearestWords(word, m_words);
        QStringList result;
        if (!m_suggest || !m_freeList)
            return result;
        char **list = nullptr;
        const int count = m_suggest(m_handle, &list, word.toUtf8().constData());
        for (int i = 0; i < count && result.size() < kMaxSuggestions; ++i)
            result.append(QString::fromUtf8(list[i]));
        if (list)
            m_freeList(m_handle, &list, count);
        return result;
    }

private:
    using CreateFunction = void *(*)(const char *, const char *);
    using DestroyFunction = void (*)(void *);
    using SpellFunction = int (*)(void *, const char *);
    using SuggestFunction = int (*)(void *, char ***, const char *);
    using FreeListFunction = void (*)(void *, char ***, int);

    void loadPlainWordList()
    {
        QStringList paths;
        for (const QString &root :
             QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
            paths << QDir(root).filePath(QStringLiteral("dict/words"))
                  << QDir(root).filePath(QStringLiteral("dict/american-english"))
                  << QDir(root).filePath(QStringLiteral("dict/british-english"));
        }
        paths << QStringLiteral("/usr/share/dict/words")
              << QStringLiteral("/usr/share/dict/american-english")
              << QStringLiteral("/usr/share/dict/british-english");
        paths.removeDuplicates();

        for (const QString &path : paths) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            while (!file.atEnd()) {
                const QString word = QString::fromUtf8(file.readLine()).trimmed();
                if (!word.isEmpty())
                    m_words.insert(normalizedWord(word));
            }
            if (!m_words.isEmpty())
                return;
        }
    }

    QLibrary m_library;
    void *m_handle = nullptr;
    CreateFunction m_create = nullptr;
    DestroyFunction m_destroy = nullptr;
    SpellFunction m_spell = nullptr;
    SuggestFunction m_suggest = nullptr;
    FreeListFunction m_freeList = nullptr;
    QSet<QString> m_words;
};

SystemDictionary &systemDictionary()
{
    static SystemDictionary dictionary;
    return dictionary;
}

const QSet<QString> &productWords()
{
    static const QSet<QString> words = {
        QStringLiteral("api"),        QStringLiteral("cli"),
        QStringLiteral("codeberg"),   QStringLiteral("codex"),
        QStringLiteral("cloudflare"), QStringLiteral("forkbot"),
        QStringLiteral("forkmesh"),   QStringLiteral("github"),
        QStringLiteral("gitlab"),     QStringLiteral("http"),
        QStringLiteral("https"),      QStringLiteral("javascript"),
        QStringLiteral("json"),       QStringLiteral("linux"),
        QStringLiteral("localhost"),  QStringLiteral("macos"),
        QStringLiteral("markdown"),   QStringLiteral("readme"),
        QStringLiteral("repo"),       QStringLiteral("repos"),
        QStringLiteral("typescript"), QStringLiteral("url"),
        QStringLiteral("urls"),       QStringLiteral("yaml")};
    return words;
}

bool isCodeLike(const QString &word)
{
    bool sawLower = false;
    for (qsizetype i = 0; i < word.size(); ++i) {
        const QChar ch = word.at(i);
        if (ch.isLower())
            sawLower = true;
        else if (i > 0 && sawLower && ch.isUpper())
            return true; // camelCase and PascalCase identifiers
    }
    return false;
}

QList<QPair<int, int>> protectedRanges(const QString &text)
{
    static const QRegularExpression protectedText(QStringLiteral(
        "(?:https?://|www\\.)\\S+|[\\w.+-]+@[\\w.-]+\\.\\p{L}{2,}|`[^`]*`"));
    QList<QPair<int, int>> ranges;
    auto matches = protectedText.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        ranges.append({match.capturedStart(), match.capturedLength()});
    }
    return ranges;
}

bool rangeContains(const QList<QPair<int, int>> &ranges, int position)
{
    for (const auto &range : ranges) {
        if (position >= range.first && position < range.first + range.second)
            return true;
    }
    return false;
}

} // namespace

PromptSpellCheckHighlighter::PromptSpellCheckHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document), m_useSystemDictionary(true)
{
}

PromptSpellCheckHighlighter::PromptSpellCheckHighlighter(
    QTextDocument *document, const QSet<QString> &acceptedWords)
    : QSyntaxHighlighter(document)
{
    for (const QString &word : acceptedWords)
        m_acceptedWords.insert(normalizedWord(word));
}

bool PromptSpellCheckHighlighter::spellCheckingAvailable() const
{
    return m_useSystemDictionary ? systemDictionary().available()
                                 : !m_acceptedWords.isEmpty();
}

bool PromptSpellCheckHighlighter::isMisspelled(const QString &word) const
{
    if (word.size() <= 1 || isCodeLike(word))
        return false;

    const QString normalized = normalizedWord(word);
    if (productWords().contains(normalized))
        return false;

    bool allUpper = word.size() > 1;
    for (const QChar ch : word) {
        if (ch.isLetter() && !ch.isUpper()) {
            allUpper = false;
            break;
        }
    }
    if (allUpper)
        return false;

    if (m_useSystemDictionary) {
        SystemDictionary &dictionary = systemDictionary();
        return dictionary.available() && !dictionary.accepts(word) &&
               !dictionary.accepts(normalized);
    }
    return !m_acceptedWords.isEmpty() && !m_acceptedWords.contains(normalized);
}

QStringList PromptSpellCheckHighlighter::suggestions(const QString &word) const
{
    if (!isMisspelled(word))
        return {};
    return m_useSystemDictionary ? systemDictionary().suggestions(word)
                                 : nearestWords(word, m_acceptedWords);
}

QList<QPair<int, int>>
PromptSpellCheckHighlighter::misspelledRanges(const QString &text) const
{
    QList<QPair<int, int>> ranges;
    if (!spellCheckingAvailable())
        return ranges;

    static const QRegularExpression wordPattern(
        QStringLiteral(R"([\p{L}][\p{L}\p{M}'\x{2019}]*)"));
    const QList<QPair<int, int>> ignored = protectedRanges(text);

    auto words = wordPattern.globalMatch(text);
    while (words.hasNext()) {
        const QRegularExpressionMatch match = words.next();
        const int start = match.capturedStart();
        if (rangeContains(ignored, start))
            continue;
        if (start > 0) {
            const QChar prefix = text.at(start - 1);
            if (prefix == QLatin1Char('@') || prefix == QLatin1Char('#') ||
                prefix == QLatin1Char('_'))
                continue;
        }
        const int end = start + match.capturedLength();
        if (end < text.size() && text.at(end) == QLatin1Char('_'))
            continue;
        if (isMisspelled(match.captured()))
            ranges.append({start, match.capturedLength()});
    }
    return ranges;
}

void PromptSpellCheckHighlighter::highlightBlock(const QString &text)
{
    QTextCharFormat misspelledFormat;
    misspelledFormat.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    misspelledFormat.setUnderlineColor(QColor(QStringLiteral("#d73a49")));
    for (const auto &range : misspelledRanges(text))
        setFormat(range.first, range.second, misspelledFormat);
}

void PromptSpellCheckHighlighter::attachTo(QPlainTextEdit *editor)
{
    if (m_editor) {
        m_editor->removeEventFilter(this);
        m_editor->viewport()->removeEventFilter(this);
    }
    m_editor = editor;
    if (!editor)
        return;
    // Mouse context menus arrive at the viewport; the menu key goes to the
    // focused editor itself.
    editor->installEventFilter(this);
    editor->viewport()->installEventFilter(this);
}

QTextCursor
PromptSpellCheckHighlighter::misspelledWordAt(const QTextCursor &cursor) const
{
    if (cursor.isNull())
        return {};
    const QTextBlock block = cursor.block();
    const int offset = cursor.position() - block.position();
    for (const auto &range : misspelledRanges(block.text())) {
        if (offset < range.first || offset > range.first + range.second)
            continue;
        QTextCursor word(block);
        word.setPosition(block.position() + range.first);
        word.setPosition(block.position() + range.first + range.second,
                         QTextCursor::KeepAnchor);
        return word;
    }
    return {};
}

QMenu *PromptSpellCheckHighlighter::createContextMenu(
    QPlainTextEdit *editor, const QTextCursor &cursor,
    const QPoint &position) const
{
    QMenu *menu = editor->createStandardContextMenu(position);
    const QTextCursor word = misspelledWordAt(cursor);
    if (word.isNull() || editor->isReadOnly())
        return menu;

    const QString original = word.selectedText();
    const int start = word.selectionStart();
    const QList<QAction *> standardActions = menu->actions();
    QAction *before = standardActions.isEmpty() ? nullptr : standardActions.first();

    const QStringList corrections = suggestions(original);
    for (const QString &correction : corrections) {
        auto *action = new QAction(
            QString(correction).replace(QLatin1Char('&'), QStringLiteral("&&")), menu);
        QFont font = action->font();
        font.setBold(true);
        action->setFont(font);
        QObject::connect(
            action, &QAction::triggered, editor,
            [editor, start, original, correction] {
                QTextCursor replace(editor->document());
                replace.setPosition(start);
                replace.setPosition(start + int(original.size()),
                                    QTextCursor::KeepAnchor);
                // The prompt may have changed while the menu was open.
                if (replace.selectedText() != original)
                    return;
                replace.insertText(correction);
                editor->setTextCursor(replace);
            });
        menu->insertAction(before, action);
    }
    if (corrections.isEmpty()) {
        auto *none = new QAction(QStringLiteral("No spelling suggestions"), menu);
        none->setEnabled(false);
        menu->insertAction(before, none);
    }
    if (before)
        menu->insertSeparator(before);
    return menu;
}

bool PromptSpellCheckHighlighter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::ContextMenu || !m_editor ||
        (watched != m_editor && watched != m_editor->viewport()))
        return QSyntaxHighlighter::eventFilter(watched, event);

    const auto *menuEvent = static_cast<QContextMenuEvent *>(event);
    QTextCursor cursor = m_editor->textCursor();
    if (watched == m_editor->viewport() &&
        menuEvent->reason() != QContextMenuEvent::Keyboard)
        cursor = m_editor->cursorForPosition(menuEvent->pos());

    QMenu *menu = createContextMenu(m_editor, cursor, menuEvent->pos());
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(menuEvent->globalPos());
    return true;
}
