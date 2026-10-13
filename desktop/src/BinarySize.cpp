#include "BinarySize.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define FORKMESH_BINARY_SIZE_CXXABI 1
#endif

namespace forkmesh::binarysize {

namespace {

constexpr quint32 kShtSymtab = 2;
constexpr quint32 kShtNobits = 8;
constexpr quint32 kShtDynsym = 11;
constexpr quint64 kShfAlloc = 2;
constexpr quint64 kShfExec = 4;
constexpr int kSttObject = 1;
constexpr int kSttFunc = 2;
constexpr int kSttFile = 4;
constexpr int kSttTls = 6;
constexpr int kSttIfunc = 10;
constexpr int kStbLocal = 0;
constexpr quint16 kShnUndef = 0;
constexpr quint16 kShnLoreserve = 0xff00;
constexpr quint16 kShnCommon = 0xfff2;
constexpr int kSymbolEntry = 24;
constexpr int kShared = -2;

struct Section {
    QByteArray name;
    quint32 type = 0;
    quint64 flags = 0;
    quint64 addr = 0;
    quint64 offset = 0;
    quint64 size = 0;
    quint32 link = 0;
};

struct Symbol {
    QByteArray name;
    quint64 value = 0;
    quint64 size = 0;
    int type = 0;
    int bind = 0;
    quint16 shndx = 0;
};

bool holdsBytes(int type)
{
    return type == kSttObject || type == kSttFunc || type == kSttTls ||
           type == kSttIfunc;
}

class ElfFile
{
public:
    bool open(const QString &path, QString *error)
    {
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::ReadOnly)) {
            *error = m_file.errorString();
            return false;
        }
        m_size = m_file.size();
        m_data = m_size > 0 ? m_file.map(0, m_size) : nullptr;
        if (!m_data || m_size < 64 || std::memcmp(m_data, "\x7f" "ELF", 4) != 0) {
            *error = QStringLiteral("not an ELF file");
            return false;
        }
        if (m_data[4] != 2 || m_data[5] != 1) {
            *error = QStringLiteral("only 64-bit little-endian ELF files are read");
            return false;
        }
        const quint64 shoff = read<quint64>(0x28);
        const quint16 shentsize = read<quint16>(0x3A);
        quint64 shnum = read<quint16>(0x3C);
        quint64 shstrndx = read<quint16>(0x3E);
        if (shoff == 0 || shentsize < 64) {
            *error = QStringLiteral("no section headers");
            return false;
        }
        if (shnum == 0)
            shnum = read<quint64>(shoff + 32);
        if (shstrndx == 0xffff)
            shstrndx = read<quint32>(shoff + 40);
        if (shoff + shnum * shentsize > quint64(m_size) || shnum > 1000000) {
            *error = QStringLiteral("section headers run past the end of the file");
            return false;
        }
        QVector<quint32> nameOffsets;
        m_sections.reserve(int(shnum));
        for (quint64 i = 0; i < shnum; ++i) {
            const quint64 at = shoff + i * shentsize;
            Section section;
            nameOffsets.append(read<quint32>(at));
            section.type = read<quint32>(at + 4);
            section.flags = read<quint64>(at + 8);
            section.addr = read<quint64>(at + 16);
            section.offset = read<quint64>(at + 24);
            section.size = read<quint64>(at + 32);
            section.link = read<quint32>(at + 40);
            m_sections.append(section);
        }
        if (shstrndx < shnum) {
            const Section names = m_sections.at(int(shstrndx));
            for (int i = 0; i < m_sections.size(); ++i)
                m_sections[i].name = string(names, nameOffsets.at(i));
        }
        return true;
    }

    const QVector<Section> &sections() const { return m_sections; }
    qint64 size() const { return m_size; }

    int symbolTable() const
    {
        int dynamic = -1;
        for (int i = 0; i < m_sections.size(); ++i) {
            if (m_sections.at(i).type == kShtSymtab)
                return i;
            if (m_sections.at(i).type == kShtDynsym)
                dynamic = i;
        }
        return dynamic;
    }

    template <typename Visit>
    void forEachSymbol(int table, Visit visit) const
    {
        if (table < 0 || table >= m_sections.size())
            return;
        const Section &symbols = m_sections.at(table);
        const Section strings = m_sections.value(int(symbols.link));
        const quint64 count = symbols.size / kSymbolEntry;
        for (quint64 i = 1; i < count; ++i) {
            const quint64 at = symbols.offset + i * kSymbolEntry;
            if (at + kSymbolEntry > quint64(m_size))
                return;
            Symbol symbol;
            const quint8 info = m_data[at + 4];
            symbol.type = info & 0xf;
            symbol.bind = info >> 4;
            symbol.shndx = read<quint16>(at + 6);
            symbol.value = read<quint64>(at + 8);
            symbol.size = read<quint64>(at + 16);
            symbol.name = string(strings, read<quint32>(at));
            visit(symbol);
        }
    }

private:
    template <typename T>
    T read(quint64 at) const
    {
        if (at + sizeof(T) > quint64(m_size))
            return T(0);
        return qFromLittleEndian<T>(m_data + at);
    }

    QByteArray string(const Section &table, quint64 at) const
    {
        if (at >= table.size || table.offset + at >= quint64(m_size))
            return {};
        const quint64 limit =
            std::min(table.size - at, quint64(m_size) - table.offset - at);
        const char *start = reinterpret_cast<const char *>(m_data + table.offset + at);
        return QByteArray(start, int(qstrnlen(start, uint(std::min<quint64>(limit, 1 << 20)))));
    }

    QFile m_file;
    const uchar *m_data = nullptr;
    qint64 m_size = 0;
    QVector<Section> m_sections;
};

QStringList splitNinja(const QByteArray &line)
{
    QStringList tokens;
    QByteArray current;
    for (int i = 0; i < line.size(); ++i) {
        const char c = line.at(i);
        if (c == '$' && i + 1 < line.size()) {
            current.append(line.at(++i));
            continue;
        }
        if (c == ' ') {
            if (!current.isEmpty())
                tokens.append(QString::fromUtf8(current));
            current.clear();
            continue;
        }
        current.append(c);
    }
    if (!current.isEmpty())
        tokens.append(QString::fromUtf8(current));
    return tokens;
}

bool isObjectName(const QString &token)
{
    return !token.startsWith(QLatin1Char('-')) &&
           (token.endsWith(QLatin1String(".o")) || token.endsWith(QLatin1String(".obj")));
}

int sectionCategory(const QByteArray &name, quint64 flags)
{
    if (name.startsWith(".debug") || name.startsWith(".zdebug"))
        return Debug;
    if (name == ".symtab" || name == ".strtab" || name == ".shstrtab")
        return SymbolNames;
    if (name == ".eh_frame" || name == ".eh_frame_hdr" || name == ".gcc_except_table")
        return Unwind;
    static const char *const dynamic[] = {".rela", ".rel.", ".dyn", ".gnu.hash", ".hash",
                                          ".gnu.version", ".plt", ".got", ".interp"};
    for (const char *prefix : dynamic) {
        if (name.startsWith(prefix))
            return Dynamic;
    }
    if (flags & kShfExec)
        return Other;
    if (flags & kShfAlloc)
        return Constants;
    return Other;
}

QString sectionDetail(int category, quint64 flags)
{
    switch (category) {
    case Constants:
        return QStringLiteral("String literals, jump tables and other data with no symbol name");
    case Unwind:
        return QStringLiteral("Tables the C++ runtime walks to unwind the stack for exceptions");
    case Dynamic:
        return QStringLiteral("What the dynamic loader needs to bind Qt, OpenSSL and libc at start-up");
    case Debug:
        return QStringLiteral("DWARF debug info: never loaded at run time, removed by strip");
    case SymbolNames:
        return QStringLiteral("Names of every function and variable, for debuggers and crash reports");
    default:
        break;
    }
    if (flags & kShfExec)
        return QStringLiteral("Code between named symbols: alignment padding and linker stubs");
    return QString();
}

struct Owned {
    QByteArray name;
    quint64 value = 0;
    quint64 size = 0;
    quint16 shndx = 0;
    int object = -1;
    bool shared = false;
    bool weak = false;
    QString file;
};

class Builder
{
public:
    explicit Builder(const QString &rootName)
    {
        Node root;
        root.name = rootName;
        nodes.append(root);
    }

    int child(int parent, const QString &name, int category = -1, const QString &detail = {})
    {
        const auto key = qMakePair(parent, name);
        const auto found = m_index.constFind(key);
        if (found != m_index.constEnd())
            return found.value();
        Node node;
        node.name = name;
        node.detail = detail;
        node.parent = parent;
        node.category = category >= 0 ? category : nodes.at(parent).category;
        const int index = nodes.size();
        nodes.append(node);
        nodes[parent].children.append(index);
        m_index.insert(key, index);
        return index;
    }

    int category(int which) { return child(0, categoryName(which), which); }

    void leaf(int parent, const QString &name, qint64 bytes, const QString &detail = {})
    {
        Node node;
        node.name = name;
        node.detail = detail;
        node.parent = parent;
        node.category = nodes.at(parent).category;
        node.bytes = bytes;
        nodes[parent].children.append(nodes.size());
        nodes.append(node);
    }

    void symbol(int parent, const QByteArray &name, qint64 bytes)
    {
        m_symbols[parent].append(qMakePair(name, bytes));
    }

    QVector<Node> finish(int keep)
    {
        for (auto it = m_symbols.begin(); it != m_symbols.end(); ++it) {
            QVector<QPair<QByteArray, qint64>> &list = it.value();
            std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            qint64 rest = 0;
            for (int i = 0; i < list.size(); ++i) {
                if (i < keep) {
                    const QString plain = demangle(list.at(i).first);
                    leaf(it.key(), shortNameOf(plain), list.at(i).second, plain);
                }
                else
                    rest += list.at(i).second;
            }
            if (rest > 0) {
                leaf(it.key(), QStringLiteral("%1 smaller symbols").arg(list.size() - keep),
                     rest);
            }
        }
        for (int i = nodes.size() - 1; i > 0; --i)
            nodes[nodes.at(i).parent].bytes += nodes.at(i).bytes;

        QVector<Node> out;
        out.reserve(nodes.size());
        QVector<QPair<int, int>> stack{{0, -1}};
        while (!stack.isEmpty()) {
            const auto [source, parent] = stack.takeLast();
            Node node = nodes.at(source);
            QVector<int> children = node.children;
            node.children.clear();
            node.parent = parent;
            const int index = out.size();
            out.append(node);
            if (parent >= 0)
                out[parent].children.append(index);
            std::sort(children.begin(), children.end(), [this](int a, int b) {
                return nodes.at(a).bytes > nodes.at(b).bytes;
            });
            for (auto it = children.crbegin(); it != children.crend(); ++it) {
                if (nodes.at(*it).bytes > 0)
                    stack.append(qMakePair(*it, index));
            }
        }
        return out;
    }

    QVector<Node> nodes;

private:
    QHash<QPair<int, QString>, int> m_index;
    QHash<int, QVector<QPair<QByteArray, qint64>>> m_symbols;
};

QString defaultComponent(const QString &path)
{
    const QString dir = QFileInfo(path).path();
    return dir == QLatin1String(".") ? QStringLiteral("(top level)") : dir;
}

QString stemOf(const QString &fileName)
{
    return fileName.section(QLatin1Char('.'), 0, 0);
}

}

QString categoryName(int category)
{
    switch (category) {
    case AppCode:
        return QStringLiteral("App code");
    case Templates:
        return QStringLiteral("Templates & inline");
    case Libraries:
        return QStringLiteral("Libraries & runtime");
    case Constants:
        return QStringLiteral("Constants & strings");
    case Unwind:
        return QStringLiteral("Unwind tables");
    case Dynamic:
        return QStringLiteral("Dynamic linking");
    case Debug:
        return QStringLiteral("Debug info");
    case SymbolNames:
        return QStringLiteral("Symbol names");
    default:
        return QStringLiteral("Other");
    }
}

QString formatBytes(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, bytes < 1024 * 1024 ? 0 : 1);
}

QString demangle(const QByteArray &name)
{
#ifdef FORKMESH_BINARY_SIZE_CXXABI
    if (name.startsWith("_Z")) {
        int status = 0;
        char *plain = abi::__cxa_demangle(name.constData(), nullptr, nullptr, &status);
        if (plain) {
            const QString text = status == 0 ? QString::fromUtf8(plain) : QString();
            std::free(plain);
            if (!text.isEmpty())
                return text;
        }
    }
#endif
    return QString::fromUtf8(name);
}

namespace {

QStringList scopeParts(const QString &demangled, QString *prefix, bool *typeSymbol)
{
    static const QString prefixes[] = {
        QStringLiteral("vtable for "),        QStringLiteral("VTT for "),
        QStringLiteral("construction vtable for "), QStringLiteral("typeinfo for "),
        QStringLiteral("typeinfo name for "), QStringLiteral("guard variable for "),
        QStringLiteral("non-virtual thunk to "), QStringLiteral("virtual thunk to "),
        QStringLiteral("covariant return thunk to "), QStringLiteral("(anonymous namespace)::"),
    };
    constexpr int kTypePrefixes = 5;
    QString text = demangled;
    for (bool stripped = true; stripped;) {
        stripped = false;
        for (int i = 0; i < int(std::size(prefixes)); ++i) {
            if (text.startsWith(prefixes[i])) {
                text = text.mid(prefixes[i].size());
                *typeSymbol = *typeSymbol || i < kTypePrefixes;
                if (!prefixes[i].endsWith(QLatin1String("::")))
                    prefix->append(prefixes[i]);
                stripped = true;
            }
        }
    }

    int depth = 0;
    int cut = text.size();
    int lastSpace = -1;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('(') && depth == 0) {
            cut = i;
            break;
        }
        if (c == QLatin1Char('<') || c == QLatin1Char('(') || c == QLatin1Char('[') ||
            c == QLatin1Char('{'))
            ++depth;
        else if (c == QLatin1Char('>') || c == QLatin1Char(')') || c == QLatin1Char(']') ||
                 c == QLatin1Char('}'))
            depth = std::max(0, depth - 1);
        else if (c == QLatin1Char(' ') && depth == 0)
            lastSpace = i;
    }
    const QString head = text.mid(lastSpace + 1, cut - lastSpace - 1);

    QStringList parts;
    depth = 0;
    int start = 0;
    for (int i = 0; i < head.size(); ++i) {
        const QChar c = head.at(i);
        if (c == QLatin1Char('<') || c == QLatin1Char('(') || c == QLatin1Char('{'))
            ++depth;
        else if (c == QLatin1Char('>') || c == QLatin1Char(')') || c == QLatin1Char('}'))
            depth = std::max(0, depth - 1);
        else if (depth == 0 && c == QLatin1Char(':') && i + 1 < head.size() &&
                 head.at(i + 1) == QLatin1Char(':')) {
            parts.append(head.mid(start, i - start));
            start = i + 2;
            ++i;
        }
    }
    parts.append(head.mid(start));
    for (QString &part : parts)
        part = part.section(QLatin1Char('<'), 0, 0).trimmed();

    return parts;
}

}

QString familyOf(const QString &demangled)
{
    QString prefix;
    bool typeSymbol = false;
    const QStringList parts = scopeParts(demangled, &prefix, &typeSymbol);
    if (parts.isEmpty() || parts.first().isEmpty())
        return QStringLiteral("(other)");
    if (parts.size() == 1)
        return typeSymbol ? parts.first() : QStringLiteral("(free functions)");
    static const QStringList nested = {
        QStringLiteral("std"), QStringLiteral("__gnu_cxx"), QStringLiteral("__cxxabiv1"),
        QStringLiteral("QtPrivate"), QStringLiteral("QtConcurrent"), QStringLiteral("forkmesh"),
    };
    if (nested.contains(parts.at(0)) && !parts.at(1).isEmpty())
        return parts.at(0) + QStringLiteral("::") + parts.at(1);
    return parts.at(0);
}

QString shortNameOf(const QString &demangled)
{
    QString prefix;
    bool typeSymbol = false;
    const QStringList parts = scopeParts(demangled, &prefix, &typeSymbol);
    if (parts.isEmpty() || parts.last().isEmpty())
        return demangled;
    return prefix + parts.mid(std::max<qsizetype>(0, parts.size() - 2)).join(QStringLiteral("::"));
}

QStringList linkedObjects(const QString &buildDir, const QString &targetName)
{
    const QDir dir(buildDir);
    QStringList found;
    const auto accept = [&](const QString &token) {
        if (!isObjectName(token))
            return;
        const QString path = QDir::cleanPath(dir.absoluteFilePath(token));
        if (!found.contains(path))
            found.append(path);
    };

    QFile ninja(dir.filePath(QStringLiteral("build.ninja")));
    if (ninja.open(QIODevice::ReadOnly)) {
        const QByteArray want = "build " + targetName.toUtf8() + ':';
        const QByteArray wantExe = "build " + targetName.toUtf8() + ".exe:";
        while (!ninja.atEnd()) {
            const QByteArray line = ninja.readLine();
            if (!line.startsWith(want) && !line.startsWith(wantExe))
                continue;
            const QStringList tokens = splitNinja(line.trimmed());
            for (int i = 3; i < tokens.size(); ++i) {
                if (tokens.at(i) == QLatin1String("|") || tokens.at(i) == QLatin1String("||"))
                    break;
                accept(tokens.at(i));
            }
            break;
        }
        if (!found.isEmpty())
            return found;
    }

    const QString targetDir =
        dir.filePath(QStringLiteral("CMakeFiles/%1.dir").arg(targetName));
    QFile link(targetDir + QStringLiteral("/link.txt"));
    if (link.open(QIODevice::ReadOnly)) {
        const QStringList tokens = QString::fromUtf8(link.readAll()).split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const QString &token : tokens)
            accept(token);
        if (!found.isEmpty())
            return found;
    }

    QDirIterator it(targetDir, {QStringLiteral("*.o"), QStringLiteral("*.obj")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        found.append(QDir::cleanPath(it.next()));
    found.sort();
    return found;
}

QString sourcePathOf(const QString &objectPath, const QString &sourceRoot)
{
    QString path = QDir::fromNativeSeparators(objectPath);
    const int marker = path.indexOf(QLatin1String(".dir/"));
    path = marker >= 0 ? path.mid(marker + 5) : QFileInfo(path).fileName();
    if (path.endsWith(QLatin1String(".obj")))
        path.chop(4);
    else if (path.endsWith(QLatin1String(".o")))
        path.chop(2);
    QString root = QDir::cleanPath(QDir::fromNativeSeparators(sourceRoot));
    while (root.startsWith(QLatin1Char('/')))
        root.remove(0, 1);
    if (!root.isEmpty() && path.startsWith(root + QLatin1Char('/')))
        path = path.mid(root.size() + 1);
    return path;
}

Report analyse(const Options &options, const std::function<bool(int, int)> &progress)
{
    QElapsedTimer clock;
    clock.start();
    Report report;
    report.binaryPath = options.binaryPath;

    ElfFile binary;
    if (!binary.open(options.binaryPath, &report.error))
        return report;
    report.fileSize = binary.size();

    const QStringList objects = options.buildDir.isEmpty()
        ? QStringList()
        : linkedObjects(options.buildDir, options.targetName);
    report.objectsListed = objects.size();
    const int total = objects.size() + 1;
    const auto componentOf = options.componentOf ? options.componentOf : defaultComponent;

    QVector<QString> sources;
    QHash<QByteArray, int> definedBy;
    QHash<QString, int> byFileName;
    for (int i = 0; i < objects.size(); ++i) {
        if (progress && !progress(i, total)) {
            report.error = QStringLiteral("cancelled");
            return report;
        }
        const QString source = sourcePathOf(objects.at(i), options.sourceRoot);
        sources.append(source);
        const QString fileName = QFileInfo(source).fileName();
        byFileName.insert(fileName, byFileName.contains(fileName) ? kShared : i);
        ElfFile object;
        QString ignored;
        if (!object.open(objects.at(i), &ignored))
            continue;
        ++report.objectCount;
        object.forEachSymbol(object.symbolTable(), [&](const Symbol &symbol) {
            if (symbol.bind == kStbLocal || symbol.shndx == kShnUndef ||
                !holdsBytes(symbol.type) || symbol.name.isEmpty())
                return;
            const auto it = definedBy.find(symbol.name);
            if (it == definedBy.end())
                definedBy.insert(symbol.name, i);
            else if (it.value() != i)
                it.value() = kShared;
        });
    }
    if (progress && !progress(objects.size(), total)) {
        report.error = QStringLiteral("cancelled");
        return report;
    }

    const auto objectForFile = [&](const QString &file) {
        const int exact = byFileName.value(file, -1);
        if (exact >= 0)
            return exact;
        const QString stem = stemOf(file);
        for (int i = 0; i < sources.size(); ++i) {
            const QString candidate = stemOf(QFileInfo(sources.at(i)).fileName());
            if (!candidate.isEmpty() && stem.startsWith(candidate + QLatin1Char('_')))
                return i;
        }
        return -1;
    };

    const QVector<Section> &sections = binary.sections();
    const int table = binary.symbolTable();
    report.fromSymtab = table >= 0 && sections.at(table).type == kShtSymtab;
    QVector<Owned> owned;
    QHash<QString, int> fileObjects;
    QString currentFile;
    binary.forEachSymbol(table, [&](const Symbol &symbol) {
        if (symbol.type == kSttFile) {
            currentFile = QString::fromUtf8(symbol.name);
            return;
        }
        if (!holdsBytes(symbol.type) || symbol.size == 0 || symbol.shndx == kShnUndef ||
            symbol.shndx >= kShnLoreserve || symbol.shndx >= sections.size() ||
            sections.at(symbol.shndx).type == kShtNobits)
            return;
        Owned entry;
        entry.name = symbol.name;
        entry.value = symbol.value;
        entry.size = symbol.size;
        entry.shndx = symbol.shndx;
        entry.weak = symbol.bind != kStbLocal && symbol.bind != 1;
        if (symbol.bind == kStbLocal) {
            entry.file = currentFile;
            if (!currentFile.isEmpty()) {
                auto it = fileObjects.find(currentFile);
                if (it == fileObjects.end())
                    it = fileObjects.insert(currentFile, objectForFile(currentFile));
                entry.object = it.value();
            }
        } else {
            const int owner = definedBy.value(symbol.name, -1);
            entry.shared = owner == kShared;
            entry.object = owner >= 0 ? owner : -1;
        }
        owned.append(entry);
    });
    report.symbolCount = owned.size();

    std::sort(owned.begin(), owned.end(), [](const Owned &a, const Owned &b) {
        if (a.shndx != b.shndx)
            return a.shndx < b.shndx;
        if (a.value != b.value)
            return a.value < b.value;
        return a.size > b.size;
    });

    Builder tree(QFileInfo(options.binaryPath).fileName());
    QVector<quint64> attributed(sections.size(), 0);
    QHash<QString, QString> families;
    const auto familyFor = [&](const QByteArray &name) {
        const QString plain = demangle(name);
        auto it = families.find(plain);
        if (it == families.end())
            it = families.insert(plain, familyOf(plain));
        return it.value();
    };
    quint64 reach = 0;
    int reachSection = -1;
    for (const Owned &entry : std::as_const(owned)) {
        const Section &section = sections.at(entry.shndx);
        if (reachSection != entry.shndx) {
            reachSection = entry.shndx;
            reach = section.addr;
        }
        const quint64 end = std::min(entry.value + entry.size, section.addr + section.size);
        const quint64 start = std::max(entry.value, reach);
        if (end <= start)
            continue;
        reach = end;
        const qint64 bytes = qint64(end - start);
        attributed[entry.shndx] += quint64(bytes);

        int group = -1;
        if (entry.object >= 0) {
            const QString &source = sources.at(entry.object);
            const int component = tree.child(tree.category(AppCode), componentOf(source));
            group = tree.child(component, QFileInfo(source).fileName(), -1, source);
        } else if (entry.shared || (objects.isEmpty() && entry.weak)) {
            group = tree.child(tree.category(Templates), familyFor(entry.name));
        } else if (objects.isEmpty() && !entry.file.isEmpty()) {
            group = tree.child(tree.child(tree.category(AppCode), componentOf(entry.file)),
                               entry.file);
        } else if (objects.isEmpty()) {
            const int unmapped = tree.child(tree.category(AppCode),
                                            QStringLiteral("Globals (no build folder)"));
            group = tree.child(unmapped, familyFor(entry.name));
        } else if (!entry.file.isEmpty()) {
            group = tree.child(tree.category(Libraries), entry.file);
        } else {
            group = tree.child(tree.category(Libraries), familyFor(entry.name));
        }
        tree.symbol(group, entry.name, bytes);
    }

    quint64 inSections = 0;
    for (int i = 1; i < sections.size(); ++i) {
        const Section &section = sections.at(i);
        if (section.type == kShtNobits || section.size == 0)
            continue;
        inSections += section.size;
        const quint64 rest = section.size - std::min(section.size, attributed.at(i));
        if (rest == 0)
            continue;
        const int category = sectionCategory(section.name, section.flags);
        tree.leaf(tree.category(category), QString::fromUtf8(section.name), qint64(rest),
                  sectionDetail(category, section.flags));
    }
    if (quint64(report.fileSize) > inSections) {
        tree.leaf(tree.category(Other), QStringLiteral("ELF headers & alignment"),
                  report.fileSize - qint64(inSections),
                  QStringLiteral("File header, program headers, section header table and "
                                 "the padding that aligns each segment to a page"));
    }

    report.nodes = tree.finish(std::max(1, options.symbolsPerGroup));
    if (!report.nodes.isEmpty())
        report.nodes[0].detail = options.binaryPath;
    if (progress)
        progress(total, total);
    report.scanMs = clock.elapsed();
    return report;
}

}
