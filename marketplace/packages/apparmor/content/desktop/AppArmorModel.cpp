#include "AppArmorModel.h"

#include <QSet>

#include <algorithm>

namespace forkmesh::apparmor {
namespace {

struct Token {
    QString text;
    qsizetype start = 0;
    qsizetype end = 0;
};

QList<Token> headerTokens(const QString &text, qsizetype offset)
{
    QList<Token> tokens;
    qsizetype i = 0;
    while (i < text.size()) {
        while (i < text.size() && text.at(i).isSpace())
            ++i;
        if (i >= text.size())
            break;
        const qsizetype start = i;
        bool quote = false;
        int parens = 0;
        while (i < text.size()) {
            const QChar c = text.at(i);
            if (quote) {
                if (c == QLatin1Char('\\'))
                    ++i;
                else if (c == QLatin1Char('"'))
                    quote = false;
            } else if (c == QLatin1Char('"')) {
                quote = true;
            } else if (c == QLatin1Char('(')) {
                ++parens;
            } else if (c == QLatin1Char(')')) {
                parens = std::max(0, parens - 1);
            } else if (c.isSpace() && parens == 0) {
                break;
            }
            ++i;
        }
        tokens << Token{text.mid(start, i - start), offset + start, offset + i};
    }
    return tokens;
}

QString unquote(const QString &value)
{
    if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
        return value.mid(1, value.size() - 2);
    return value;
}

bool isModeFlag(const QString &flag)
{
    static const QSet<QString> modes = {QStringLiteral("complain"), QStringLiteral("enforce"), QStringLiteral("kill"),
                                        QStringLiteral("unconfined"), QStringLiteral("prompt")};
    return modes.contains(flag);
}

QStringList parseFlags(const QString &token)
{
    QString body = token.mid(token.indexOf(QLatin1Char('=')) + 1).trimmed();
    if (body.startsWith(QLatin1Char('(')))
        body = body.mid(1);
    if (body.endsWith(QLatin1Char(')')))
        body.chop(1);
    QStringList flags;
    for (const QString &part : body.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts))
        flags << part.trimmed();
    return flags;
}

bool fillHeader(ProfileDeclaration &declaration, const QString &text, qsizetype lineStart, qsizetype brace)
{
    const QString header = text.mid(lineStart, brace - lineStart);
    const QList<Token> tokens = headerTokens(header, lineStart);
    if (tokens.isEmpty())
        return false;
    int index = 0;
    if (tokens.first().text == QStringLiteral("profile")) {
        if (tokens.size() < 2)
            return false;
        declaration.name = unquote(tokens.at(1).text);
        index = 2;
        if (index < tokens.size() && !tokens.at(index).text.contains(QLatin1Char('=')))
            declaration.attachment = unquote(tokens.at(index++).text);
    } else {
        const QString first = tokens.first().text;
        if (!first.startsWith(QLatin1Char('/')) && !first.startsWith(QStringLiteral("@{")) && !first.startsWith(QLatin1Char('"'))
            && !first.startsWith(QLatin1Char(':')))
            return false;
        declaration.name = unquote(first);
        declaration.attachment = declaration.name;
        index = 1;
    }
    for (; index < tokens.size(); ++index) {
        const Token &token = tokens.at(index);
        if (token.text.startsWith(QStringLiteral("flags="))) {
            declaration.flags = parseFlags(token.text);
            declaration.flagsStart = token.start;
            declaration.flagsEnd = token.end;
        }
    }
    declaration.headerStart = lineStart + (tokens.first().start - lineStart);
    return !declaration.name.isEmpty();
}

QString localIncludeIn(const QString &body, const QString &name)
{
    static const QRegularExpression include(QStringLiteral("^\\s*#?include\\s+(?:if\\s+exists\\s+)?<local/([^>]+)>"),
                                            QRegularExpression::MultilineOption);
    QString first;
    auto it = include.globalMatch(body);
    while (it.hasNext()) {
        const QString found = it.next().captured(1).trimmed();
        if (found == name || found == fileNameForProfile(name))
            return found;
        if (first.isEmpty())
            first = found;
    }
    return first;
}

QString escapePath(const QString &path)
{
    QString escaped;
    for (const QChar c : path) {
        if (QStringLiteral("*?[]{}\"\\^").contains(c))
            escaped += QLatin1Char('\\');
        escaped += c;
    }
    if (escaped.contains(QRegularExpression(QStringLiteral("[\\s,#]"))))
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    return escaped;
}

QString filePermissions(const QString &mask)
{
    QString perms;
    if (mask.contains(QLatin1Char('m')))
        perms += QLatin1Char('m');
    if (mask.contains(QLatin1Char('r')))
        perms += QLatin1Char('r');
    if (mask.contains(QLatin1Char('w')) || mask.contains(QLatin1Char('c')) || mask.contains(QLatin1Char('d')))
        perms += QLatin1Char('w');
    else if (mask.contains(QLatin1Char('a')))
        perms += QLatin1Char('a');
    if (mask.contains(QLatin1Char('l')))
        perms += QLatin1Char('l');
    if (mask.contains(QLatin1Char('k')))
        perms += QLatin1Char('k');
    if (mask.contains(QLatin1Char('x')))
        perms += QStringLiteral("ix");
    return perms;
}

QString decodeHex(const QString &value)
{
    static const QRegularExpression hex(QStringLiteral("^(?:[0-9A-F]{2})+$"));
    if (!hex.match(value).hasMatch())
        return value;
    return QString::fromUtf8(QByteArray::fromHex(value.toLatin1()));
}

bool isPlainWord(const QString &value)
{
    static const QRegularExpression word(QStringLiteral("^[A-Za-z0-9_.:/@+-]+$"));
    return word.match(value).hasMatch();
}

}

QString modeName(Mode mode)
{
    switch (mode) {
    case Mode::Enforce: return QStringLiteral("enforce");
    case Mode::Complain: return QStringLiteral("complain");
    case Mode::Kill: return QStringLiteral("kill");
    case Mode::Unconfined: return QStringLiteral("unconfined");
    case Mode::Prompt: return QStringLiteral("prompt");
    case Mode::Mixed: return QStringLiteral("mixed");
    case Mode::Unknown: return QStringLiteral("unknown");
    }
    return {};
}

Mode modeFromText(const QString &text)
{
    const QString value = text.trimmed().toLower();
    for (Mode mode : {Mode::Enforce, Mode::Complain, Mode::Kill, Mode::Unconfined, Mode::Prompt, Mode::Mixed}) {
        if (modeName(mode) == value)
            return mode;
    }
    return Mode::Unknown;
}

QList<LoadedProfile> parseLoadedProfiles(const QString &text)
{
    QList<LoadedProfile> profiles;
    for (const QString &raw : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        const qsizetype open = line.lastIndexOf(QStringLiteral(" ("));
        if (open <= 0 || !line.endsWith(QLatin1Char(')')))
            continue;
        profiles << LoadedProfile{line.left(open), modeFromText(line.mid(open + 2, line.size() - open - 3))};
    }
    return profiles;
}

QList<ProfileDeclaration> parseProfileDeclarations(const QString &text)
{
    QList<ProfileDeclaration> declarations;
    int depth = 0;
    int glob = 0;
    bool recording = false;
    ProfileDeclaration current;
    qsizetype lineStart = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\n')) {
            glob = 0;
            lineStart = i + 1;
            continue;
        }
        if (c == QLatin1Char('"')) {
            for (++i; i < text.size() && text.at(i) != QLatin1Char('"') && text.at(i) != QLatin1Char('\n'); ++i) {
                if (text.at(i) == QLatin1Char('\\'))
                    ++i;
            }
            continue;
        }
        if (c == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if (c == QLatin1Char('#') && (i == lineStart || text.at(i - 1).isSpace())) {
            while (i + 1 < text.size() && text.at(i + 1) != QLatin1Char('\n'))
                ++i;
            continue;
        }
        if (c == QLatin1Char('{')) {
            const bool blockOpen = glob == 0 && (i + 1 >= text.size() || text.at(i + 1).isSpace() || text.at(i + 1) == QLatin1Char('#'))
                && i > lineStart && text.at(i - 1).isSpace();
            if (!blockOpen) {
                ++glob;
                continue;
            }
            if (depth == 0) {
                current = ProfileDeclaration{};
                recording = fillHeader(current, text, lineStart, i);
                current.bodyOpen = i;
            }
            ++depth;
            continue;
        }
        if (c == QLatin1Char('}')) {
            if (glob > 0) {
                --glob;
                continue;
            }
            if (depth == 0)
                continue;
            --depth;
            if (depth == 0 && recording) {
                current.bodyClose = i;
                current.localInclude = localIncludeIn(text.mid(current.bodyOpen, i - current.bodyOpen), current.name);
                declarations << current;
                recording = false;
            }
        }
    }
    return declarations;
}

Mode declaredMode(const QStringList &flags)
{
    for (const QString &flag : flags) {
        if (flag == QStringLiteral("complain"))
            return Mode::Complain;
        if (flag == QStringLiteral("kill"))
            return Mode::Kill;
        if (flag == QStringLiteral("unconfined"))
            return Mode::Unconfined;
        if (flag == QStringLiteral("prompt"))
            return Mode::Prompt;
    }
    return Mode::Enforce;
}

Mode fileMode(const QList<ProfileDeclaration> &profiles)
{
    if (profiles.isEmpty())
        return Mode::Unknown;
    const Mode first = declaredMode(profiles.first().flags);
    for (const ProfileDeclaration &profile : profiles) {
        if (declaredMode(profile.flags) != first)
            return Mode::Mixed;
    }
    return first;
}

QString withMode(const QString &text, Mode mode)
{
    QString result = text;
    QList<ProfileDeclaration> declarations = parseProfileDeclarations(text);
    std::reverse(declarations.begin(), declarations.end());
    for (const ProfileDeclaration &declaration : declarations) {
        QStringList flags;
        if (mode == Mode::Complain)
            flags << QStringLiteral("complain");
        else if (mode == Mode::Kill)
            flags << QStringLiteral("kill");
        for (const QString &flag : declaration.flags) {
            if (!isModeFlag(flag))
                flags << flag;
        }
        if (declaration.flagsStart >= 0) {
            if (flags.isEmpty()) {
                qsizetype start = declaration.flagsStart;
                while (start > declaration.headerStart && result.at(start - 1).isSpace() && result.at(start - 1) != QLatin1Char('\n'))
                    --start;
                result.remove(start, declaration.flagsEnd - start);
            } else {
                result.replace(declaration.flagsStart, declaration.flagsEnd - declaration.flagsStart,
                               QStringLiteral("flags=(%1)").arg(flags.join(QLatin1Char(','))));
            }
        } else if (!flags.isEmpty()) {
            qsizetype at = declaration.bodyOpen;
            while (at > declaration.headerStart && result.at(at - 1).isSpace())
                --at;
            result.insert(at, QStringLiteral(" flags=(%1)").arg(flags.join(QLatin1Char(','))));
        }
    }
    return result;
}

QString withRule(const QString &text, const QString &profileName, const QString &rule)
{
    for (const ProfileDeclaration &declaration : parseProfileDeclarations(text)) {
        if (declaration.name != profileName && declaration.attachment != profileName)
            continue;
        QString result = text;
        qsizetype lineStart = result.lastIndexOf(QLatin1Char('\n'), declaration.bodyClose - 1) + 1;
        if (result.mid(lineStart, declaration.bodyClose - lineStart).trimmed().isEmpty())
            result.insert(lineStart, QStringLiteral("  %1\n").arg(rule.trimmed()));
        else
            result.insert(declaration.bodyClose, QStringLiteral("\n  %1\n").arg(rule.trimmed()));
        return result;
    }
    return {};
}

bool containsRule(const QString &text, const QString &rule)
{
    const QString wanted = rule.trimmed();
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (line.trimmed() == wanted)
            return true;
    }
    return false;
}

QString withLocalRule(const QString &localText, const QString &rule)
{
    if (containsRule(localText, rule))
        return localText;
    QString result = localText;
    if (!result.isEmpty() && !result.endsWith(QLatin1Char('\n')))
        result += QLatin1Char('\n');
    return result + rule.trimmed() + QLatin1Char('\n');
}

QString newProfileTemplate(const QString &name, const QString &program, const QString &abi)
{
    const QString path = escapePath(program);
    QString text;
    if (!abi.isEmpty())
        text += QStringLiteral("abi <abi/%1>,\n").arg(abi);
    text += QStringLiteral("include <tunables/global>\n\n");
    text += QStringLiteral("profile %1 %2 flags=(complain) {\n").arg(name, path);
    text += QStringLiteral("  include <abstractions/base>\n\n");
    text += QStringLiteral("  %1 mr,\n\n").arg(path);
    text += QStringLiteral("  include if exists <local/%1>\n").arg(name);
    text += QStringLiteral("}\n");
    return text;
}

bool isSafeProfileName(const QString &name)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$"));
    return safe.match(name).hasMatch() && !name.contains(QStringLiteral(".."));
}

bool isSafeFileName(const QString &name)
{
    return isSafeProfileName(name) && isPolicyFileName(name);
}

QString fileNameForProfile(const QString &name)
{
    if (!name.startsWith(QLatin1Char('/')))
        return name;
    QString file = name.mid(1);
    file.replace(QLatin1Char('/'), QLatin1Char('.'));
    return file;
}

bool isPolicyFileName(const QString &name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('~')))
        return false;
    static const QStringList skipped = {QStringLiteral(".dpkg-new"), QStringLiteral(".dpkg-old"), QStringLiteral(".dpkg-dist"),
                                        QStringLiteral(".dpkg-bak"), QStringLiteral(".dpkg-remove"), QStringLiteral(".rpmnew"),
                                        QStringLiteral(".rpmsave"), QStringLiteral(".orig"), QStringLiteral(".rej"),
                                        QStringLiteral(".pacnew"), QStringLiteral(".pacsave"), QStringLiteral(".md"),
                                        QStringLiteral(".bak"), QStringLiteral(".swp")};
    for (const QString &suffix : skipped) {
        if (name.endsWith(suffix))
            return false;
    }
    return name != QStringLiteral("README");
}

QString globPattern(const QString &attachment)
{
    QString pattern = QStringLiteral("^");
    int alternation = 0;
    for (qsizetype i = 0; i < attachment.size(); ++i) {
        const QChar c = attachment.at(i);
        if (c == QLatin1Char('\\') && i + 1 < attachment.size()) {
            pattern += QRegularExpression::escape(QString(attachment.at(++i)));
        } else if (c == QLatin1Char('@') && attachment.mid(i, 2) == QStringLiteral("@{")) {
            const qsizetype close = attachment.indexOf(QLatin1Char('}'), i);
            if (close < 0)
                return {};
            pattern += QStringLiteral(".+");
            i = close;
        } else if (c == QLatin1Char('*')) {
            if (attachment.mid(i, 2) == QStringLiteral("**")) {
                pattern += QStringLiteral(".*");
                ++i;
            } else {
                pattern += QStringLiteral("[^/]*");
            }
        } else if (c == QLatin1Char('?')) {
            pattern += QStringLiteral("[^/]");
        } else if (c == QLatin1Char('{')) {
            pattern += QStringLiteral("(?:");
            ++alternation;
        } else if (c == QLatin1Char(',') && alternation > 0) {
            pattern += QLatin1Char('|');
        } else if (c == QLatin1Char('}') && alternation > 0) {
            pattern += QLatin1Char(')');
            --alternation;
        } else if (c == QLatin1Char('[')) {
            const qsizetype close = attachment.indexOf(QLatin1Char(']'), i + 1);
            if (close < 0)
                return {};
            QString cls = attachment.mid(i + 1, close - i - 1);
            cls.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
            pattern += QLatin1Char('[') + cls + QLatin1Char(']');
            i = close;
        } else {
            pattern += QRegularExpression::escape(QString(c));
        }
    }
    if (alternation != 0)
        return {};
    return pattern + QLatin1Char('$');
}

bool attachmentMatches(const QString &attachment, const QString &path)
{
    if (attachment.isEmpty() || path.isEmpty() || !attachment.startsWith(QLatin1Char('/')))
        return false;
    const QString pattern = globPattern(attachment);
    if (pattern.isEmpty())
        return false;
    const QRegularExpression expression(pattern);
    return expression.isValid() && expression.match(path).hasMatch();
}

std::optional<Event> parseEvent(const QString &line)
{
    const qsizetype marker = line.indexOf(QStringLiteral("apparmor="));
    if (marker < 0)
        return std::nullopt;
    Event event;
    event.raw = line.trimmed();
    static const QRegularExpression stamp(QStringLiteral("audit\\((\\d+)\\.(\\d+):(\\d+)\\)"));
    const QRegularExpressionMatch time = stamp.match(line);
    if (time.hasMatch()) {
        event.time = QDateTime::fromMSecsSinceEpoch(time.captured(1).toLongLong() * 1000 + time.captured(2).left(3).toInt());
        event.key = time.captured(1) + QLatin1Char('.') + time.captured(2) + QLatin1Char(':') + time.captured(3);
    } else {
        event.key = event.raw;
    }
    static const QRegularExpression field(QStringLiteral("([A-Za-z_][A-Za-z0-9_]*)=(\"([^\"]*)\"|[^\\s]+)"));
    static const QSet<QString> encoded = {QStringLiteral("name"), QStringLiteral("comm"), QStringLiteral("profile"),
                                          QStringLiteral("peer"), QStringLiteral("target"), QStringLiteral("label"),
                                          QStringLiteral("peer_label"), QStringLiteral("name2"), QStringLiteral("exe")};
    auto it = field.globalMatch(line, marker);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString key = match.captured(1);
        QString value = match.captured(2);
        if (value.startsWith(QLatin1Char('"')))
            value = match.captured(3);
        else if (encoded.contains(key))
            value = decodeHex(value);
        if (!event.fields.contains(key))
            event.fields.insert(key, value);
    }
    event.result = event.fields.value(QStringLiteral("apparmor")).toUpper();
    event.operation = event.fields.value(QStringLiteral("operation"));
    event.profile = event.fields.value(QStringLiteral("profile"));
    event.name = event.fields.value(QStringLiteral("name"));
    event.comm = event.fields.value(QStringLiteral("comm"));
    event.pid = event.fields.value(QStringLiteral("pid")).toLongLong();
    event.requested = event.fields.value(QStringLiteral("requested_mask"), event.fields.value(QStringLiteral("requested")));
    event.denied = event.fields.value(QStringLiteral("denied_mask"), event.fields.value(QStringLiteral("denied")));
    event.className = event.fields.value(QStringLiteral("class"));
    event.capability = event.fields.value(QStringLiteral("capname"));
    event.family = event.fields.value(QStringLiteral("family"));
    event.sockType = event.fields.value(QStringLiteral("sock_type"));
    event.info = event.fields.value(QStringLiteral("info"));
    event.target = eventTarget(event);
    if (event.result.isEmpty())
        return std::nullopt;
    return event;
}

QList<Event> parseEvents(const QString &text)
{
    QList<Event> events;
    QSet<QString> seen;
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        std::optional<Event> event = parseEvent(line);
        if (!event || seen.contains(event->key))
            continue;
        seen.insert(event->key);
        events << *event;
    }
    std::stable_sort(events.begin(), events.end(), [](const Event &left, const Event &right) { return left.time > right.time; });
    return events;
}

QString eventTarget(const Event &event)
{
    if (!event.capability.isEmpty())
        return QStringLiteral("capability %1").arg(event.capability);
    if (!event.family.isEmpty())
        return QStringLiteral("network %1 %2").arg(event.family, event.sockType).trimmed();
    const QString peer = event.fields.value(QStringLiteral("peer"));
    if (event.operation == QStringLiteral("signal"))
        return QStringLiteral("signal %1 to %2").arg(event.fields.value(QStringLiteral("signal")), peer);
    if (event.operation == QStringLiteral("ptrace"))
        return QStringLiteral("ptrace %1").arg(peer);
    if (event.operation.startsWith(QStringLiteral("dbus")))
        return QStringLiteral("dbus %1.%2").arg(event.fields.value(QStringLiteral("interface")), event.fields.value(QStringLiteral("member")));
    return event.name;
}

QString topLevelProfile(const QString &profile)
{
    const qsizetype child = profile.indexOf(QStringLiteral("//"));
    return child < 0 ? profile : profile.left(child);
}

QString suggestedRule(const Event &event)
{
    if (event.result != QStringLiteral("DENIED") && event.result != QStringLiteral("ALLOWED"))
        return {};
    const QString mask = event.denied.isEmpty() ? event.requested : event.denied;
    if (!event.capability.isEmpty()) {
        if (!isPlainWord(event.capability))
            return {};
        return QStringLiteral("capability %1,").arg(event.capability);
    }
    if (!event.family.isEmpty()) {
        if (!isPlainWord(event.family) || (!event.sockType.isEmpty() && !isPlainWord(event.sockType)))
            return {};
        return QStringLiteral("network %1 %2,").arg(event.family, event.sockType).replace(QStringLiteral(" ,"), QStringLiteral(","));
    }
    const QString peer = event.fields.value(QStringLiteral("peer"));
    if (event.operation == QStringLiteral("signal")) {
        const QString signal = event.fields.value(QStringLiteral("signal"));
        if (!isPlainWord(mask) || !isPlainWord(signal) || !isPlainWord(peer))
            return {};
        return QStringLiteral("signal (%1) set=(%2) peer=%3,").arg(mask, signal, peer);
    }
    if (event.operation == QStringLiteral("ptrace")) {
        if (!isPlainWord(mask) || !isPlainWord(peer))
            return {};
        return QStringLiteral("ptrace (%1) peer=%2,").arg(mask, peer);
    }
    if (event.operation.startsWith(QStringLiteral("dbus"))) {
        const QString direction = event.fields.value(QStringLiteral("mask"));
        const QString bus = event.fields.value(QStringLiteral("bus"));
        const QString path = event.fields.value(QStringLiteral("path"));
        const QString interface = event.fields.value(QStringLiteral("interface"));
        const QString member = event.fields.value(QStringLiteral("member"));
        const QString destination = event.fields.value(QStringLiteral("name"));
        for (const QString &part : {direction, bus, path, interface, member}) {
            if (!isPlainWord(part))
                return {};
        }
        QString rule = QStringLiteral("dbus %1 bus=%2 path=%3 interface=%4 member=%5").arg(direction, bus, path, interface, member);
        if (!destination.isEmpty() && isPlainWord(destination))
            rule += QStringLiteral(" peer=(name=%1)").arg(destination);
        return rule + QLatin1Char(',');
    }
    if (event.name.startsWith(QLatin1Char('/')) && !mask.isEmpty()) {
        const QString perms = filePermissions(mask);
        if (perms.isEmpty() || event.name.contains(QLatin1Char('\n')))
            return {};
        return QStringLiteral("%1 %2,").arg(escapePath(event.name), perms);
    }
    return {};
}

ProcessLabel parseLabel(const QString &current)
{
    QString label = current.trimmed();
    while (label.endsWith(QChar(0)))
        label.chop(1);
    if (label.isEmpty())
        return {};
    if (label == QStringLiteral("unconfined"))
        return ProcessLabel{QString(), Mode::Unconfined};
    static const QRegularExpression withMode(QStringLiteral("^(.*) \\(([a-z]+)\\)$"));
    const QRegularExpressionMatch match = withMode.match(label);
    if (match.hasMatch())
        return ProcessLabel{match.captured(1), modeFromText(match.captured(2))};
    return ProcessLabel{label, Mode::Unknown};
}

QHash<QString, QString> splitSections(const QString &output)
{
    QHash<QString, QString> sections;
    QString current;
    for (const QString &line : output.split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral("@@"))) {
            current = line.mid(2).trimmed();
            sections.insert(current, QString());
            continue;
        }
        if (current.isEmpty())
            continue;
        sections[current] += line + QLatin1Char('\n');
    }
    return sections;
}

QList<HourBucket> hourlyBuckets(const QList<Event> &events, const QDateTime &end, int hours)
{
    QList<HourBucket> buckets;
    if (hours <= 0)
        return buckets;
    QDateTime last = end;
    last.setTime(QTime(end.time().hour(), 0));
    const QDateTime first = last.addSecs(-3600LL * (hours - 1));
    for (int i = 0; i < hours; ++i)
        buckets << HourBucket{first.addSecs(3600LL * i)};
    for (const Event &event : events) {
        if (!event.time.isValid())
            continue;
        const qint64 offset = first.secsTo(event.time);
        if (offset < 0)
            continue;
        const qint64 index = offset / 3600;
        if (index >= hours)
            continue;
        if (event.result == QStringLiteral("DENIED"))
            ++buckets[index].denied;
        else if (event.result == QStringLiteral("ALLOWED"))
            ++buckets[index].allowed;
    }
    return buckets;
}

}
