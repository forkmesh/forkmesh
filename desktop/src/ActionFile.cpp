#include "ActionFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>

namespace {

struct Node {
    enum Type { Null, Scalar, Map, Seq };
    Type type = Null;
    QString scalar;
    QMap<QString, Node> map;
    QList<Node> seq;

    const Node *child(const QString &key) const
    {
        auto it = map.constFind(key);
        return it == map.constEnd() ? nullptr : &it.value();
    }
};

struct Line {
    int indent;
    QString text; // content with indentation stripped, never empty
};

int leadingSpaces(const QString &raw)
{
    int n = 0;
    while (n < raw.size() && raw.at(n) == QLatin1Char(' '))
        ++n;
    return n;
}

QList<Line> splitLines(const QString &content)
{
    QList<Line> out;
    const QStringList raw = content.split(QLatin1Char('\n'));
    for (QString line : raw) {
        line.replace(QLatin1Char('\t'), QStringLiteral("    "));
        while (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;
        Line l;
        l.indent = leadingSpaces(line);
        l.text = line.mid(l.indent);
        out.append(l);
    }
    return out;
}

QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 &&
        ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) ||
         (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))) {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

Node parseFlowSeq(const QString &inner)
{
    Node node;
    node.type = Node::Seq;
    const QStringList parts = inner.split(QLatin1Char(','));
    for (const QString &part : parts) {
        const QString v = unquote(part);
        if (v.isEmpty())
            continue;
        Node item;
        item.type = Node::Scalar;
        item.scalar = v;
        node.seq.append(item);
    }
    return node;
}

QString captureBlockScalar(const QList<Line> &lines, int &pos, int parentIndent)
{
    QStringList block;
    int blockIndent = -1;
    while (pos < lines.size() && lines.at(pos).indent > parentIndent) {
        const Line &l = lines.at(pos);
        if (blockIndent < 0)
            blockIndent = l.indent;
        const int strip = qMin(blockIndent, l.indent);
        block.append(QString(l.indent - strip, QLatin1Char(' ')) + l.text);
        ++pos;
    }
    return block.join(QLatin1Char('\n'));
}

Node parseBlock(const QList<Line> &lines, int &pos, int indent);

Node parseNestedValue(const QList<Line> &lines, int &pos, int parentIndent)
{
    if (pos >= lines.size() || lines.at(pos).indent <= parentIndent) {
        Node empty;
        return empty; // Null
    }
    return parseBlock(lines, pos, lines.at(pos).indent);
}

void parseMapEntry(const QList<Line> &lines, int &pos, int indent, Node &map)
{
    const QString text = lines.at(pos).text;
    const int colon = text.indexOf(QLatin1Char(':'));
    if (colon < 0) {
        ++pos; // not a key: skip defensively
        return;
    }
    const QString key = unquote(text.left(colon));
    QString rest = text.mid(colon + 1).trimmed();
    ++pos;

    Node value;
    if (rest == QLatin1String("|") || rest == QLatin1String("|-") ||
        rest == QLatin1String(">") || rest == QLatin1String(">-")) {
        value.type = Node::Scalar;
        value.scalar = captureBlockScalar(lines, pos, indent);
    } else if (rest.startsWith(QLatin1Char('[')) && rest.endsWith(QLatin1Char(']'))) {
        value = parseFlowSeq(rest.mid(1, rest.size() - 2));
    } else if (!rest.isEmpty()) {
        value.type = Node::Scalar;
        value.scalar = unquote(rest);
    } else {
        value = parseNestedValue(lines, pos, indent);
    }
    map.map.insert(key, value);
}

Node parseBlock(const QList<Line> &lines, int &pos, int indent)
{
    Node node;
    const bool isSeq = lines.at(pos).text.startsWith(QLatin1String("- ")) ||
                       lines.at(pos).text == QLatin1String("-");
    node.type = isSeq ? Node::Seq : Node::Map;

    while (pos < lines.size() && lines.at(pos).indent == indent) {
        const Line &line = lines.at(pos);
        if (node.type == Node::Seq) {
            if (!line.text.startsWith(QLatin1Char('-')))
                break;
            const QString after = line.text.mid(1).trimmed();
            if (after.contains(QLatin1Char(':')) &&
                !after.startsWith(QLatin1Char('['))) {
                const int dashCols = line.text.indexOf(after.at(0));
                QList<Line> synthetic = lines;
                Line first;
                first.indent = indent + dashCols;
                first.text = after;
                synthetic[pos] = first;
                int sub = pos;
                Node item = parseBlock(synthetic, sub, first.indent);
                node.seq.append(item);
                pos = sub;
            } else if (!after.isEmpty()) {
                Node item;
                item.type = Node::Scalar;
                item.scalar = unquote(after);
                node.seq.append(item);
                ++pos;
            } else {
                ++pos;
                node.seq.append(parseNestedValue(lines, pos, indent));
            }
        } else {
            parseMapEntry(lines, pos, indent, node);
        }
    }
    return node;
}

QStringList scalarOrList(const Node *node)
{
    QStringList out;
    if (!node)
        return out;
    if (node->type == Node::Scalar)
        out.append(node->scalar.trimmed());
    else if (node->type == Node::Seq)
        for (const Node &item : node->seq)
            if (item.type == Node::Scalar)
                out.append(item.scalar.trimmed());
    return out;
}

static QList<QRegularExpression> compiledPathPatterns(const QStringList &patterns)
{
    QList<QRegularExpression> compiled;
    for (const QString &pattern : patterns) {
        const QString normalized = pattern.trimmed();
        if (normalized.isEmpty())
            continue;
        QRegularExpression re(
            QRegularExpression::wildcardToRegularExpression(normalized));
        if (!re.isValid())
            continue;
        re.optimize();
        compiled.append(re);
    }
    return compiled;
}

// `on:` accepts three shapes. The scalar and sequence forms are just event
// names, but the mapping form carries per-event filters
// (`on:\n  pull_request:\n    branches: [main]`) and its *keys* are the events.
// Reading only scalars there used to leave `on` empty, so a workflow written
// the mapping way triggered on nothing at all.
QStringList eventNames(const Node *node)
{
    QStringList out;
    if (!node)
        return out;
    if (node->type == Node::Map) {
        for (auto it = node->map.constBegin(); it != node->map.constEnd(); ++it) {
            const QString event = it.key().trimmed();
            if (!event.isEmpty() && !out.contains(event))
                out.append(event);
        }
        return out;
    }
    for (const QString &event : scalarOrList(node))
        if (!event.isEmpty() && !out.contains(event))
            out.append(event);
    return out;
}

constexpr int kMaxWorkflowMinutes = 1440;

bool workflowMinutes(const Node *node, int *minutes)
{
    *minutes = 0;
    const QString text = node ? node->scalar.trimmed() : QString();
    if (text.isEmpty())
        return true;
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value < 1 || value > kMaxWorkflowMinutes)
        return false;
    *minutes = value;
    return true;
}

void collectSteps(const Node *stepsNode, QList<ActionStep> &steps, bool *badTimeout)
{
    if (!stepsNode || stepsNode->type != Node::Seq)
        return;
    for (const Node &item : stepsNode->seq) {
        if (item.type != Node::Map)
            continue;
        ActionStep step;
        if (const Node *n = item.child(QStringLiteral("name")))
            step.name = n->scalar.trimmed();
        if (const Node *r = item.child(QStringLiteral("run")))
            step.run = r->scalar;
        if (!workflowMinutes(item.child(QStringLiteral("timeout-minutes")),
                             &step.timeoutMinutes))
            *badTimeout = true;
        if (!step.run.isEmpty())
            steps.append(step);
    }
}

const QString kAnyNodeLabel = QStringLiteral("any");

} // namespace

bool ActionWorkflow::matchesPushChangedPaths(const QStringList &changedPaths) const
{
    if (pushChangedPaths.isEmpty())
        return true;
    if (changedPaths.isEmpty())
        return false;

    const QList<QRegularExpression> patterns =
        compiledPathPatterns(pushChangedPaths);
    for (const QString &changedPath : changedPaths)
        for (const QRegularExpression &pattern : patterns)
            if (pattern.match(changedPath).hasMatch())
                return true;

    return false;
}

bool ActionWorkflow::runsOnNode(const QStringList &nodeLabels) const
{
    if (runsOn.isEmpty())
        return true; // undedicated: runs wherever the push is seen
    for (const QString &wanted : runsOn) {
        if (wanted == kAnyNodeLabel)
            return true;
        for (const QString &have : nodeLabels)
            if (QString::compare(wanted, have.trimmed(), Qt::CaseInsensitive) == 0)
                return true;
    }
    return false;
}

QStringList ActionFile::parseLabelList(const QString &configured)
{
    QStringList out;
    static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    const QStringList parts =
        configured.split(separators, Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString label = part.trimmed().toLower();
        if (!label.isEmpty() && !out.contains(label))
            out.append(label);
    }
    return out;
}

QString ActionFile::pinnedNode(const QStringList &pins, const QString &path)
{
    if (path.isEmpty())
        return QString();
    for (const QString &entry : pins) {
        const int sep = entry.indexOf(QLatin1Char('\t'));
        if (sep <= 0)
            continue;
        if (entry.left(sep) == path)
            return entry.mid(sep + 1).trimmed().toLower();
    }
    return QString();
}

QStringList ActionFile::setPinnedNode(const QStringList &pins,
                                      const QStringList &paths,
                                      const QString &node)
{
    QMap<QString, QString> byPath; // sorted, so a rewrite is stable
    for (const QString &entry : pins) {
        const int sep = entry.indexOf(QLatin1Char('\t'));
        if (sep > 0)
            byPath.insert(entry.left(sep), entry.mid(sep + 1).trimmed().toLower());
    }
    const QString label = node.trimmed().toLower();
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        if (label.isEmpty())
            byPath.remove(path);
        else
            byPath.insert(path, label);
    }
    QStringList out;
    for (auto it = byPath.cbegin(); it != byPath.cend(); ++it)
        out.append(it.key() + QLatin1Char('\t') + it.value());
    return out;
}

QStringList ActionFile::nodeLabels(const QString &machineNode,
                                   const QString &mirrorNode,
                                   const QString &configured)
{
    QStringList out;
    const auto add = [&out](const QString &raw) {
        const QString label = raw.trimmed().toLower();
        if (!label.isEmpty() && !out.contains(label))
            out.append(label);
    };
    add(machineNode);
    add(mirrorNode);
#if defined(Q_OS_MACOS)
    add(QStringLiteral("macos"));
#elif defined(Q_OS_WIN)
    add(QStringLiteral("windows"));
#elif defined(Q_OS_LINUX)
    add(QStringLiteral("linux"));
#endif
    for (const QString &label : parseLabelList(configured))
        add(label);
    return out;
}

ActionWorkflow ActionFile::parse(const QString &relPath, const QString &content)
{
    ActionWorkflow wf;
    wf.path = relPath;
    wf.content = content;
    wf.name = QFileInfo(relPath).fileName();

    QList<Line> lines = splitLines(content);
    if (lines.isEmpty()) {
        wf.error = QStringLiteral("empty workflow file");
        return wf;
    }

    int pos = 0;
    Node root = parseBlock(lines, pos, lines.first().indent);
    if (root.type != Node::Map) {
        wf.error = QStringLiteral("workflow root must be a mapping");
        return wf;
    }

    if (const Node *n = root.child(QStringLiteral("name")))
        if (!n->scalar.trimmed().isEmpty())
            wf.name = n->scalar.trimmed();

    wf.on = eventNames(root.child(QStringLiteral("on")));
    if (const Node *onNode = root.child(QStringLiteral("on"));
        onNode && onNode->type == Node::Map) {
        for (auto it = onNode->map.constBegin(); it != onNode->map.constEnd();
             ++it) {
            if (it.key().trimmed() != QStringLiteral("push"))
                continue;
            if (const Node *pathsNode = it.value().child(QStringLiteral("paths")))
                for (const QString &pattern : scalarOrList(pathsNode)) {
                    const QString normalized = pattern.trimmed();
                    if (!normalized.isEmpty() &&
                        !wf.pushChangedPaths.contains(normalized))
                        wf.pushChangedPaths.append(normalized);
                }
        }
    }

    const auto addRunsOn = [&wf](const Node *node) {
        for (const QString &label : scalarOrList(node)) {
            const QString normalized = label.trimmed().toLower();
            if (!normalized.isEmpty() && !wf.runsOn.contains(normalized))
                wf.runsOn.append(normalized);
        }
    };
    addRunsOn(root.child(QStringLiteral("runs-on")));
    if (const Node *checkout = root.child(QStringLiteral("checkout"))) {
        const QString value = checkout->scalar.trimmed().toLower();
        wf.checkout = value != QLatin1String("false") &&
                      value != QLatin1String("no") &&
                      value != QLatin1String("off") &&
                      value != QLatin1String("0");
    }
    if (const Node *sandbox = root.child(QStringLiteral("sandbox"))) {
        const QString value = sandbox->scalar.trimmed().toLower();
        wf.sandbox = value != QLatin1String("false") &&
                     value != QLatin1String("no") &&
                     value != QLatin1String("off") &&
                     value != QLatin1String("0");
    }
    if (const Node *depth = root.child(QStringLiteral("fetch-depth"))) {
        bool ok = false;
        const int value = depth->scalar.trimmed().toInt(&ok);
        if (ok && value >= 0)
            wf.fetchDepth = value;
    }

    QStringList jobIds;
    if (const Node *jobs = root.child(QStringLiteral("jobs")))
        if (jobs->type == Node::Map)
            for (auto it = jobs->map.constBegin(); it != jobs->map.constEnd(); ++it)
                jobIds.append(it.key().trimmed());
    const auto addNeeds = [&wf, &jobIds](const Node *node) {
        for (const QString &raw : scalarOrList(node)) {
            const QString token = raw.trimmed();
            if (token.isEmpty())
                continue;
            bool sameFileJob = false;
            for (const QString &job : jobIds)
                if (QString::compare(job, token, Qt::CaseInsensitive) == 0)
                    sameFileJob = true;
            if (sameFileJob || wf.needs.contains(token))
                continue;
            wf.needs.append(token);
        }
    };
    addNeeds(root.child(QStringLiteral("needs")));

    if (const Node *env = root.child(QStringLiteral("env")))
        if (env->type == Node::Map)
            for (auto it = env->map.constBegin(); it != env->map.constEnd(); ++it)
                wf.env.insert(it.key(), it.value().scalar);

    bool badTimeout = false;
    collectSteps(root.child(QStringLiteral("steps")), wf.steps, &badTimeout);
    if (const Node *jobs = root.child(QStringLiteral("jobs")))
        if (jobs->type == Node::Map)
            for (auto it = jobs->map.constBegin(); it != jobs->map.constEnd(); ++it) {
                addRunsOn(it.value().child(QStringLiteral("runs-on")));
                addNeeds(it.value().child(QStringLiteral("needs")));
                collectSteps(it.value().child(QStringLiteral("steps")), wf.steps,
                             &badTimeout);
            }

    if (badTimeout) {
        wf.error = QStringLiteral("workflow timeout-minutes is not a whole number of minutes");
        return wf;
    }
    if (wf.steps.isEmpty()) {
        wf.error = QStringLiteral("workflow has no runnable steps");
        return wf;
    }
    if (!wf.sandbox && (wf.on != QStringList{QStringLiteral("workflow_dispatch")})) {
        wf.error = QStringLiteral("sandbox: false is only allowed with on: "
                                  "workflow_dispatch and no other trigger");
        return wf;
    }
    wf.valid = true;
    return wf;
}

QList<ActionWorkflow> ActionFile::parseWorkflowsInDir(
    const QString &checkoutDir)
{
    QList<ActionWorkflow> out;
    QDir dir(checkoutDir + QStringLiteral("/.forkmesh"));
    if (!dir.exists())
        return out;
    const QStringList files =
        dir.entryList({QStringLiteral("*.yml"), QStringLiteral("*.yaml")},
                      QDir::Files, QDir::Name);
    for (const QString &file : files) {
        QFile f(dir.filePath(file));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString content = QString::fromUtf8(f.readAll());
        out.append(parse(QStringLiteral(".forkmesh/") + file, content));
    }
    return out;
}

QString ActionFile::substitute(const QString &input,
                               const QMap<QString, QString> &vars)
{
    QString result = input;
    static const QRegularExpression ctx(
        QStringLiteral("\\$\\{\\{\\s*vars\\.([A-Za-z_][A-Za-z0-9_]*)\\s*\\}\\}"));
    static const QRegularExpression brace(
        QStringLiteral("\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}"));
    struct Pass {
        const QRegularExpression &re;
        bool onlyKnown;
    };
    for (const Pass &pass : {Pass{ctx, false}, Pass{brace, true}}) {
        QString out;
        int last = 0;
        auto it = pass.re.globalMatch(result);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString name = m.captured(1);
            if (pass.onlyKnown && !vars.contains(name))
                continue; // leave shell variables for the shell
            out += result.mid(last, m.capturedStart() - last);
            out += vars.value(name);
            last = m.capturedEnd();
        }
        out += result.mid(last);
        result = out;
    }
    return result;
}

bool ActionFile::defaultBranchRetiresPush(const QString &relPath,
                                          const QString &defaultBranchContent)
{
    if (defaultBranchContent.trimmed().isEmpty())
        return false;
    const ActionWorkflow onDefault = parse(relPath, defaultBranchContent);
    return onDefault.valid && !onDefault.triggersOnPush();
}
