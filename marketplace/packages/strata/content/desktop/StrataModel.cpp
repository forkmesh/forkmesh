#include "StrataModel.h"

#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace forkmesh::strata {

using insight::FileInfo;
using insight::FunctionInfo;
using insight::ModuleEdge;
using insight::ModuleGraph;
using insight::ModuleNode;
using insight::Snapshot;

namespace {

enum RuleId {
    ModuleCycles,
    FileCycles,
    StableDependencies,
    ZoneOfPain,
    ZoneOfUselessness,
    HubFiles,
    TooManyImports,
    ComplexFunctions,
    LongFunctions,
    DeepNesting,
    TooManyParameters,
    BrainMethods,
    LargeFiles,
    DuplicatedCode,
    UncommentedComplexity,
    KnowledgeIslands,
    FormerContributors,
    RuleCount
};

double capped(double value, double cap)
{
    return std::clamp(value, 0.0, cap);
}

QString fileName(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? path : path.mid(slash + 1);
}

int severityIndex(Severity severity)
{
    return int(severity);
}

QString esc(const QString &text)
{
    return text.toHtmlEscaped();
}

}

int Rule::worst() const
{
    int best = -1;
    for (const Violation &violation : violations) {
        const int index = severityIndex(violation.severity);
        if (best < 0 || index < best)
            best = index;
    }
    return best;
}

int Report::ruleIndex(const QString &id) const
{
    for (int i = 0; i < rules.size(); ++i) {
        if (rules[i].id == id)
            return i;
    }
    return -1;
}

int Report::gatesWith(GateStatus status) const
{
    int count = 0;
    for (const GateResult &gate : gates) {
        if (gate.status == status)
            ++count;
    }
    return count;
}

const Violation &Report::violation(const ViolationRef &ref) const
{
    return rules[ref.first].violations[ref.second];
}

QVector<ViolationRef> Report::worstForModule(int module, int limit) const
{
    QVector<ViolationRef> refs;
    if (module < 0 || module >= graph.nodes.size())
        return refs;
    if (module < moduleViolations.size())
        refs += moduleViolations[module];
    for (int file : graph.nodes[module].files) {
        if (file >= 0 && file < fileViolations.size())
            refs += fileViolations[file];
    }
    std::stable_sort(refs.begin(), refs.end(), [this](const ViolationRef &a, const ViolationRef &b) {
        const Violation &va = violation(a);
        const Violation &vb = violation(b);
        if (va.severity != vb.severity)
            return va.severity < vb.severity;
        return va.debt > vb.debt;
    });
    if (limit > 0 && refs.size() > limit)
        refs.resize(limit);
    return refs;
}

double effortMinutes(int codeLines)
{
    return std::max(0, codeLines) * 18.0 * 8.0 * 60.0 / 1000.0;
}

int ratingFor(double ratio)
{
    if (ratio <= 0.05)
        return 0;
    if (ratio <= 0.10)
        return 1;
    if (ratio <= 0.20)
        return 2;
    if (ratio <= 0.50)
        return 3;
    return 4;
}

QChar ratingLetter(int rating)
{
    return QChar(QLatin1Char(char('A' + std::clamp(rating, 0, 4))));
}

QString severityName(Severity severity)
{
    switch (severity) {
    case Severity::Critical:
        return QStringLiteral("Critical");
    case Severity::High:
        return QStringLiteral("High");
    case Severity::Medium:
        return QStringLiteral("Medium");
    case Severity::Low:
        return QStringLiteral("Low");
    }
    return {};
}

QString categoryName(Category category)
{
    switch (category) {
    case Category::Architecture:
        return QStringLiteral("Architecture");
    case Category::Design:
        return QStringLiteral("Design");
    case Category::CodeSmells:
        return QStringLiteral("Code smells");
    case Category::Maintainability:
        return QStringLiteral("Maintainability");
    }
    return {};
}

QString gateStatusName(GateStatus status)
{
    switch (status) {
    case GateStatus::Pass:
        return QStringLiteral("Pass");
    case GateStatus::Warn:
        return QStringLiteral("Warn");
    case GateStatus::Fail:
        return QStringLiteral("Fail");
    }
    return {};
}

QString formatDebt(double minutes)
{
    const qint64 total = std::max<qint64>(0, qint64(std::llround(minutes)));
    if (total < 60)
        return QStringLiteral("%1 min").arg(total);
    const qint64 hours = total / 60;
    const qint64 rest = total % 60;
    if (hours < 8)
        return rest ? QStringLiteral("%1 h %2 min").arg(hours).arg(rest)
                    : QStringLiteral("%1 h").arg(hours);
    const qint64 days = hours / 8;
    const qint64 hourRest = hours % 8;
    const QString dayText = QLocale().toString(days);
    if (days >= 100 || hourRest == 0)
        return QStringLiteral("%1 d").arg(dayText);
    return QStringLiteral("%1 d %2 h").arg(dayText).arg(hourRest);
}

QString formatPercent(double ratio)
{
    const double percent = ratio * 100.0;
    if (percent > 0 && percent < 0.1)
        return QStringLiteral("<0.1 %");
    return QStringLiteral("%1 %").arg(QLocale().toString(percent, 'f', percent >= 10 ? 0 : 1));
}

QList<Rule> ruleCatalog()
{
    QList<Rule> rules;
    rules.reserve(RuleCount);
    const auto add = [&rules](const char *id, const char *title, Category category,
                              Severity severity, bool moduleScope, const char *description,
                              const char *fix) {
        Rule rule;
        rule.id = QString::fromLatin1(id);
        rule.title = QString::fromUtf8(title);
        rule.category = category;
        rule.severity = severity;
        rule.moduleScope = moduleScope;
        rule.description = QString::fromUtf8(description);
        rule.howToFix = QString::fromUtf8(fix);
        rules.append(rule);
    };
    add("ARC01", "Avoid module dependency cycles", Category::Architecture, Severity::Critical, true,
        "Modules that depend on each other, directly or through other modules, can only be "
        "understood, tested, built and released together. A cycle turns separate layers into "
        "one big tangle and every change risks rippling around the loop.",
        "Find the weakest edge of the cycle (fewest imports, shown in red) and remove it: move "
        "the shared code down into a lower module, or invert the dependency with an interface "
        "or callback owned by the lower module.");
    add("ARC02", "Avoid file import cycles", Category::Architecture, Severity::High, false,
        "Files that import each other in a loop cannot be read, reused or tested on their own "
        "and make initialization order fragile.",
        "Extract the definitions both files need into a third file that imports neither, or "
        "pass the dependency in instead of importing it.");
    add("ARC03", "Depend in the direction of stability", Category::Architecture, Severity::Medium, true,
        "Stable dependencies principle: a module should only depend on modules that are more "
        "stable than itself. When a stable module (many dependents, few dependencies) depends on a "
        "clearly less stable one, changes in the volatile module ripple into everything that "
        "relies on the stable one.",
        "Move the code the stable module needs into it or into a lower module, or let the stable "
        "module define an abstraction that the volatile module implements.");
    add("ARC04", "Modules in the zone of pain", Category::Architecture, Severity::Medium, true,
        "A module that is concrete (few abstract types) and stable (used by many, depends on few) "
        "is hard to change because every dependent breaks, yet offers no abstractions to extend "
        "it. It sits far below the main sequence in the abstractness × instability diagram.",
        "Introduce interfaces or abstract types that dependents use instead of the concrete "
        "code, or split the module so that fewer modules depend on the concrete parts.");
    add("ARC05", "Modules in the zone of uselessness", Category::Architecture, Severity::Low, true,
        "A module that is mostly abstract yet has no dependents is abstraction nobody uses: "
        "interfaces without callers, speculative extension points.",
        "Remove the unused abstractions, or merge them into the module that implements them.");
    add("DES01", "Avoid hub files", Category::Design, Severity::Medium, false,
        "A file that many files import and that itself imports many files is a hub: every change "
        "to its dependencies can break its many dependents. Hubs are usually god objects or "
        "grab-bag utility files.",
        "Split the file by responsibility so that each dependent imports only the part it needs.");
    add("DES02", "Avoid too many imports in one file", Category::Design, Severity::Low, false,
        "A file with a very long import list knows too much about the rest of the system and "
        "is likely doing several jobs.",
        "Split the file along its responsibilities, or group related imports behind a facade.");
    add("CS01", "Avoid complex functions", Category::CodeSmells, Severity::High, false,
        "Cyclomatic complexity counts the independent paths through a function. Above 10 a "
        "function becomes hard to test fully; above 20 it is hard to understand at all.",
        "Extract the branches into well-named helper functions, replace conditionals with "
        "lookup tables or polymorphism, and return early instead of nesting.");
    add("CS02", "Avoid long functions", Category::CodeSmells, Severity::Medium, false,
        "Functions longer than 60 lines rarely do one thing; above 100 lines they are hard to "
        "name, read and reuse.",
        "Extract cohesive blocks into functions whose names say what the block does.");
    add("CS03", "Avoid deep nesting", Category::CodeSmells, Severity::Medium, false,
        "Code nested more than five levels deep forces the reader to hold every enclosing "
        "condition in mind.",
        "Return early, invert conditions, and extract inner loops into their own functions.");
    add("CS04", "Avoid functions with too many parameters", Category::CodeSmells, Severity::Low, false,
        "More than six parameters make calls hard to read and easy to get wrong.",
        "Group related parameters into a struct or options object, or split the function.");
    add("CS05", "Avoid brain methods", Category::CodeSmells, Severity::High, false,
        "A brain method is long, complex and deeply nested at the same time: it centralizes the "
        "logic of its file and is where most bugs hide.",
        "Break it up step by step: extract each nested block into a named function, then "
        "simplify the remaining control flow.");
    add("MNT01", "Avoid large files", Category::Maintainability, Severity::Medium, false,
        "Files with more than 1,000 lines of code are hard to navigate and attract merge "
        "conflicts; above 2,000 lines they usually hold several unrelated concerns.",
        "Split the file by responsibility, one concept per file.");
    add("MNT02", "Avoid duplicated code", Category::Maintainability, Severity::Medium, false,
        "Copied blocks must be fixed in every copy; a bug fixed in one copy lives on in the "
        "others.",
        "Extract the repeated block into a shared function and call it from each place.");
    add("MNT03", "Comment complex files", Category::Maintainability, Severity::Low, false,
        "A complex file with almost no comments or documentation leaves its reasoning in its "
        "author's head.",
        "Document why the code is shaped the way it is, near the code or in the project's docs.");
    add("MNT04", "Avoid knowledge islands", Category::Maintainability, Severity::Medium, false,
        "One person wrote more than 90 % of a file that changes often. When they are away, "
        "nobody else knows the code.",
        "Pair on the next changes, review them with a second person, and write down how the "
        "file works.");
    add("MNT05", "Hotspots written by former contributors", Category::Maintainability, Severity::Medium, false,
        "Most of this frequently changed code was written by people who have not committed in "
        "the last six months. The knowledge behind it has left with them.",
        "Give the file a current owner, cover it with tests before changing it, and document "
        "what you learn.");
    return rules;
}

QList<QVector<int>> components(const QList<QVector<int>> &adjacency)
{
    const int n = adjacency.size();
    QVector<int> index(n, -1);
    QVector<int> low(n, 0);
    QVector<char> onStack(n, 0);
    QVector<int> stack;
    QVector<QPair<int, int>> calls;
    QList<QVector<int>> out;
    int counter = 0;
    for (int start = 0; start < n; ++start) {
        if (index[start] >= 0)
            continue;
        index[start] = low[start] = counter++;
        stack.append(start);
        onStack[start] = 1;
        calls.append({start, 0});
        while (!calls.isEmpty()) {
            const int v = calls.last().first;
            const int next = calls.last().second;
            if (next < adjacency[v].size()) {
                ++calls.last().second;
                const int w = adjacency[v][next];
                if (w < 0 || w >= n)
                    continue;
                if (index[w] < 0) {
                    index[w] = low[w] = counter++;
                    stack.append(w);
                    onStack[w] = 1;
                    calls.append({w, 0});
                } else if (onStack[w]) {
                    low[v] = std::min(low[v], index[w]);
                }
                continue;
            }
            calls.removeLast();
            if (!calls.isEmpty()) {
                const int parent = calls.last().first;
                low[parent] = std::min(low[parent], low[v]);
            }
            if (low[v] == index[v]) {
                QVector<int> component;
                int w = -1;
                do {
                    w = stack.takeLast();
                    onStack[w] = 0;
                    component.append(w);
                } while (w != v);
                std::sort(component.begin(), component.end());
                out.append(component);
            }
        }
    }
    return out;
}

void normalizeGraph(ModuleGraph &graph)
{
    const int n = graph.nodes.size();
    QList<QVector<int>> adjacency(n);
    for (const ModuleEdge &edge : graph.edges) {
        if (edge.from >= 0 && edge.from < n && edge.to >= 0 && edge.to < n && edge.from != edge.to)
            adjacency[edge.from].append(edge.to);
    }
    const QList<QVector<int>> parts = components(adjacency);
    graph.cycles.clear();
    for (ModuleNode &node : graph.nodes)
        node.cycle = -1;
    for (const QVector<int> &part : parts) {
        if (part.size() < 2)
            continue;
        for (int v : part)
            graph.nodes[v].cycle = graph.cycles.size();
        graph.cycles.append(part);
    }
    for (ModuleEdge &edge : graph.edges) {
        edge.inCycle = edge.from >= 0 && edge.from < n && edge.to >= 0 && edge.to < n
                       && graph.nodes[edge.from].cycle >= 0
                       && graph.nodes[edge.from].cycle == graph.nodes[edge.to].cycle;
    }
    bool coupled = false;
    for (const ModuleNode &node : graph.nodes) {
        if (node.afferent > 0 || node.efferent > 0) {
            coupled = true;
            break;
        }
    }
    if (!coupled && !graph.edges.isEmpty()) {
        QVector<QSet<int>> in(n), out(n);
        for (const ModuleEdge &edge : graph.edges) {
            if (edge.from < 0 || edge.from >= n || edge.to < 0 || edge.to >= n || edge.from == edge.to)
                continue;
            out[edge.from].insert(edge.to);
            in[edge.to].insert(edge.from);
        }
        for (int i = 0; i < n; ++i) {
            ModuleNode &node = graph.nodes[i];
            node.afferent = in[i].size();
            node.efferent = out[i].size();
            const int sum = node.afferent + node.efferent;
            node.instability = sum ? double(node.efferent) / sum : 0;
            node.abstractness = node.types ? double(node.abstractTypes) / node.types : 0;
            node.distance = std::abs(node.abstractness + node.instability - 1.0);
        }
    }
}

QList<Rule> evaluateRules(const Snapshot &snapshot, const ModuleGraph &graph)
{
    QList<Rule> rules = ruleCatalog();
    const int fileCount = snapshot.files.size();
    const auto moduleOf = [&graph](int file) {
        return file >= 0 && file < graph.moduleOfFile.size() ? graph.moduleOfFile[file] : -1;
    };
    const auto add = [&rules](int rule, Violation violation) {
        violation.debt = std::round(violation.debt);
        rules[rule].violations.append(violation);
    };

    for (const ModuleEdge &edge : graph.edges) {
        if (!edge.inCycle || edge.from < 0 || edge.to < 0)
            continue;
        const ModuleNode &from = graph.nodes[edge.from];
        const ModuleNode &to = graph.nodes[edge.to];
        Violation v;
        v.module = edge.from;
        v.peerModule = edge.to;
        v.file = edge.fileEdges.isEmpty() ? -1 : edge.fileEdges.first().first;
        v.severity = Severity::Critical;
        v.message = QStringLiteral("%1 uses %2 (%3 imports) and %2 depends back on %1")
                        .arg(from.name, to.name)
                        .arg(edge.weight);
        v.debt = capped(30 + 15.0 * std::max(1, edge.weight), 480);
        add(ModuleCycles, v);
    }

    {
        QList<QVector<int>> adjacency;
        adjacency.reserve(fileCount);
        for (const FileInfo &file : snapshot.files)
            adjacency.append(file.dependsOn);
        const QList<QVector<int>> parts = components(adjacency);
        for (const QVector<int> &part : parts) {
            if (part.size() < 2)
                continue;
            const QSet<int> members(part.begin(), part.end());
            for (int f : part) {
                QStringList names;
                int inside = 0;
                for (int d : snapshot.files[f].dependsOn) {
                    if (!members.contains(d))
                        continue;
                    ++inside;
                    if (names.size() < 3)
                        names << fileName(snapshot.files[d].path);
                }
                Violation v;
                v.file = f;
                v.module = moduleOf(f);
                v.severity = Severity::High;
                v.message = QStringLiteral("Part of an import cycle of %1 files; imports %2")
                                .arg(part.size())
                                .arg(names.join(QStringLiteral(", "))
                                     + (inside > names.size() ? QStringLiteral(" …") : QString()));
                v.debt = capped(15.0 * inside + 5.0 * std::min<int>(part.size(), 20), 240);
                add(FileCycles, v);
            }
        }
    }

    for (const ModuleEdge &edge : graph.edges) {
        if (edge.inCycle || edge.from < 0 || edge.to < 0)
            continue;
        const ModuleNode &from = graph.nodes[edge.from];
        const ModuleNode &to = graph.nodes[edge.to];
        const double gap = to.instability - from.instability;
        if (gap <= 0.25)
            continue;
        Violation v;
        v.module = edge.from;
        v.peerModule = edge.to;
        v.file = edge.fileEdges.isEmpty() ? -1 : edge.fileEdges.first().first;
        v.severity = Severity::Medium;
        v.message = QStringLiteral("%1 (I = %2) depends on the less stable %3 (I = %4)")
                        .arg(from.name, QString::number(from.instability, 'f', 2), to.name,
                             QString::number(to.instability, 'f', 2));
        v.debt = capped(20 + 120.0 * (gap - 0.25) + 4.0 * edge.weight, 240);
        add(StableDependencies, v);
    }

    for (int i = 0; i < graph.nodes.size(); ++i) {
        const ModuleNode &node = graph.nodes[i];
        if (node.types < 1)
            continue;
        if (node.codeLines >= 150 && node.afferent >= 2 && node.abstractness <= 0.2
            && node.instability <= 0.3) {
            const double over = std::clamp((node.distance - 0.5) / 0.5, 0.0, 1.0);
            Violation v;
            v.module = i;
            v.severity = Severity::Medium;
            v.message = QStringLiteral("Concrete and stable: A = %1, I = %2, used by %3 modules")
                            .arg(QString::number(node.abstractness, 'f', 2),
                                 QString::number(node.instability, 'f', 2))
                            .arg(node.afferent);
            v.debt = capped(30 + 150.0 * over * std::min(3.0, std::sqrt(node.codeLines / 1000.0))
                                + 5.0 * node.afferent,
                            960);
            add(ZoneOfPain, v);
        }
        if (node.codeLines >= 50 && node.abstractness >= 0.7 && node.instability >= 0.7) {
            Violation v;
            v.module = i;
            v.severity = Severity::Low;
            v.message = QStringLiteral("Abstract and unused: A = %1, I = %2")
                            .arg(QString::number(node.abstractness, 'f', 2),
                                 QString::number(node.instability, 'f', 2));
            v.debt = capped(20 + 100.0 * std::clamp((node.distance - 0.4) / 0.6, 0.0, 1.0), 240);
            add(ZoneOfUselessness, v);
        }
    }

    QVector<int> changed;
    for (const FileInfo &file : snapshot.files) {
        if (file.commits > 0)
            changed.append(file.commits);
    }
    std::sort(changed.begin(), changed.end());
    const int frequent = std::max(8, changed.isEmpty() ? 0 : changed[int(changed.size() * 0.75)]);
    const bool team = snapshot.authors.size() >= 2;

    for (int f = 0; f < fileCount; ++f) {
        const FileInfo &file = snapshot.files[f];
        const int module = moduleOf(f);
        const auto fileViolation = [&](Severity severity, int line, const QString &message, double debt) {
            Violation v;
            v.file = f;
            v.module = module;
            v.line = line;
            v.severity = severity;
            v.message = message;
            v.debt = debt;
            return v;
        };
        const int fanIn = file.dependedOnBy.size();
        const int fanOut = file.dependsOn.size();
        if (fanIn >= 10 && fanOut >= 10) {
            add(HubFiles, fileViolation(fanIn >= 25 && fanOut >= 25 ? Severity::High : Severity::Medium, 0,
                                        QStringLiteral("Imported by %1 files and imports %2").arg(fanIn).arg(fanOut),
                                        capped(30 + 3.0 * ((fanIn - 10) + (fanOut - 10)), 480)));
        }
        const int imports = std::max<int>(file.imports.size(), fanOut + file.externalImports.size());
        if (imports > 25) {
            add(TooManyImports, fileViolation(imports > 50 ? Severity::Medium : Severity::Low, 1,
                                              QStringLiteral("%1 imports").arg(imports),
                                              capped(10 + 2.0 * (imports - 25), 240)));
        }
        for (const FunctionInfo &fn : file.functions) {
            const QString name = fn.name.isEmpty() ? QStringLiteral("(anonymous)") : fn.name;
            if (fn.complexity > 10) {
                add(ComplexFunctions, fileViolation(fn.complexity > 20 ? Severity::High : Severity::Medium, fn.line,
                                                    QStringLiteral("%1 has cyclomatic complexity %2").arg(name).arg(fn.complexity),
                                                    capped(15 + 6.0 * (fn.complexity - 10), 480)));
            }
            if (fn.length > 60) {
                add(LongFunctions, fileViolation(fn.length > 100 ? Severity::Medium : Severity::Low, fn.line,
                                                 QStringLiteral("%1 is %2 lines long").arg(name).arg(fn.length),
                                                 capped(10 + 0.6 * (fn.length - 60), 480)));
            }
            if (fn.nesting > 5) {
                add(DeepNesting, fileViolation(fn.nesting > 7 ? Severity::High : Severity::Medium, fn.line,
                                               QStringLiteral("%1 nests %2 levels deep").arg(name).arg(fn.nesting),
                                               capped(20 + 15.0 * (fn.nesting - 5), 240)));
            }
            if (fn.parameters > 6) {
                add(TooManyParameters, fileViolation(fn.parameters > 9 ? Severity::Medium : Severity::Low, fn.line,
                                                     QStringLiteral("%1 takes %2 parameters").arg(name).arg(fn.parameters),
                                                     capped(10 + 6.0 * (fn.parameters - 6), 120)));
            }
            if (fn.length > 80 && fn.complexity > 15 && fn.nesting >= 4) {
                add(BrainMethods, fileViolation(Severity::High, fn.line,
                                                QStringLiteral("%1: %2 lines, complexity %3, nesting %4")
                                                    .arg(name).arg(fn.length).arg(fn.complexity).arg(fn.nesting),
                                                capped(60 + 0.5 * (fn.length - 80) + 5.0 * (fn.complexity - 15)
                                                           + 15.0 * (fn.nesting - 4),
                                                       960)));
            }
        }
        if (file.codeLines > 1000) {
            add(LargeFiles, fileViolation(file.codeLines > 2000 ? Severity::High : Severity::Medium, 1,
                                          QStringLiteral("%1 lines of code").arg(QLocale().toString(file.codeLines)),
                                          capped(30 + 0.06 * (file.codeLines - 1000), 960)));
        }
        if (file.duplicatedLines >= 12) {
            add(DuplicatedCode, fileViolation(file.duplicatedLines >= 100 ? Severity::Medium : Severity::Low, 1,
                                              QStringLiteral("%1 duplicated lines").arg(file.duplicatedLines),
                                              capped(10 + 1.5 * file.duplicatedLines, 960)));
        }
        const int commented = file.codeLines + file.commentLines;
        if (!file.test && file.complexity >= 40 && file.codeLines >= 150 && commented > 0
            && double(file.commentLines) / commented < 0.05) {
            add(UncommentedComplexity, fileViolation(Severity::Low, 1,
                                                     QStringLiteral("Complexity %1 with %2 comment lines")
                                                         .arg(file.complexity).arg(file.commentLines),
                                                     capped(15 + 0.25 * (file.complexity - 40), 240)));
        }
        if (team && file.commits >= frequent && file.mainAuthorShare > 0.9 && !file.mainAuthor.isEmpty()) {
            add(KnowledgeIslands, fileViolation(Severity::Medium, 0,
                                                QStringLiteral("%1 wrote %2 of %3 commits' lines")
                                                    .arg(file.mainAuthor, formatPercent(file.mainAuthorShare))
                                                    .arg(file.commits),
                                                capped(30 + 4.0 * (file.commits - frequent) + 0.02 * file.codeLines, 480)));
        }
        if (file.commits > 0 && file.formerAuthorShare > 0.5 && file.hotspot >= 0.3) {
            const bool severe = file.formerAuthorShare > 0.8 && file.hotspot > 0.6;
            add(FormerContributors, fileViolation(severe ? Severity::High : Severity::Medium, 0,
                                                  QStringLiteral("%1 written by former contributors, hotspot %2")
                                                      .arg(formatPercent(file.formerAuthorShare),
                                                           QString::number(file.hotspot, 'f', 2)),
                                                  capped(20 + 200.0 * file.hotspot * file.formerAuthorShare, 480)));
        }
    }

    for (Rule &rule : rules) {
        double sum = 0;
        for (const Violation &v : rule.violations)
            sum += v.debt;
        rule.debt = sum;
        std::stable_sort(rule.violations.begin(), rule.violations.end(),
                         [](const Violation &a, const Violation &b) {
                             if (a.severity != b.severity)
                                 return a.severity < b.severity;
                             return a.debt > b.debt;
                         });
    }
    return rules;
}

QList<GateResult> evaluateGates(const Snapshot &snapshot, const Report &report)
{
    QList<GateResult> gates;
    {
        GateResult gate;
        gate.name = QStringLiteral("Technical debt rating");
        gate.status = report.total.rating >= 4 ? GateStatus::Fail
                      : report.total.rating == 3 ? GateStatus::Warn
                                                 : GateStatus::Pass;
        gate.measured = QStringLiteral("%1 · %2").arg(ratingLetter(report.total.rating)).arg(formatPercent(report.total.ratio));
        gate.detail = QStringLiteral("Warns at D (debt above 20 % of the effort), fails at E (above 50 %).");
        gates.append(gate);
    }
    {
        int critical = 0;
        int rules = 0;
        for (const Rule &rule : report.rules) {
            int here = 0;
            for (const Violation &v : rule.violations) {
                if (v.severity == Severity::Critical)
                    ++here;
            }
            critical += here;
            rules += here ? 1 : 0;
        }
        GateResult gate;
        gate.name = QStringLiteral("No critical rule violated");
        gate.status = critical ? GateStatus::Fail : GateStatus::Pass;
        gate.measured = critical ? QStringLiteral("%1 in %2 %3").arg(critical).arg(rules).arg(rules == 1 ? QStringLiteral("rule") : QStringLiteral("rules"))
                                 : QStringLiteral("None");
        gate.detail = QStringLiteral("Fails when any violation has critical severity.");
        gates.append(gate);
    }
    {
        GateResult gate;
        gate.name = QStringLiteral("No module dependency cycle");
        const int cycles = report.graph.cycles.size();
        gate.status = cycles ? GateStatus::Fail : GateStatus::Pass;
        gate.measured = cycles ? QStringLiteral("%1 %2").arg(cycles).arg(cycles == 1 ? QStringLiteral("cycle") : QStringLiteral("cycles"))
                               : QStringLiteral("None");
        gate.detail = QStringLiteral("Fails when modules depend on each other in a loop.");
        gates.append(gate);
    }
    {
        GateResult gate;
        gate.name = QStringLiteral("No function above complexity 50");
        const int worst = report.metrics.maxComplexity;
        gate.status = worst > 50 ? GateStatus::Fail : worst > 30 ? GateStatus::Warn : GateStatus::Pass;
        gate.measured = report.metrics.maxComplexityFunction.isEmpty()
                            ? QString::number(worst)
                            : QStringLiteral("%1 (%2)").arg(worst).arg(report.metrics.maxComplexityFunction);
        gate.detail = QStringLiteral("Warns above 30, fails above 50.");
        gates.append(gate);
    }
    {
        GateResult gate;
        gate.name = QStringLiteral("Debt in the most-changed files");
        QVector<int> changed;
        for (int i = 0; i < snapshot.files.size(); ++i) {
            if (snapshot.files[i].commits > 0)
                changed.append(i);
        }
        if (changed.isEmpty()) {
            gate.status = GateStatus::Pass;
            gate.measured = QStringLiteral("No history");
            gate.detail = QStringLiteral("Needs git history: the debt ratio of the 10 % most-changed files must stay below 20 %.");
        } else {
            std::stable_sort(changed.begin(), changed.end(), [&snapshot](int a, int b) {
                return snapshot.files[a].commits > snapshot.files[b].commits;
            });
            const int take = std::max(1, int(std::ceil(changed.size() * 0.1)));
            double debt = 0;
            double effort = 0;
            for (int i = 0; i < take; ++i) {
                const int f = changed[i];
                if (f < report.files.size()) {
                    debt += report.files[f].debt;
                    effort += report.files[f].effort;
                }
            }
            const double ratio = effort > 0 ? debt / effort : 0;
            gate.status = ratio >= 0.2 ? GateStatus::Fail : ratio >= 0.1 ? GateStatus::Warn : GateStatus::Pass;
            gate.measured = QStringLiteral("%1 in %2 files").arg(formatPercent(ratio)).arg(take);
            gate.detail = QStringLiteral("Debt ratio of the 10 % most-changed files: warns at 10 %, fails at 20 %.");
        }
        gates.append(gate);
    }
    return gates;
}

Report buildReport(const Snapshot &snapshot, const ModuleGraph &graph)
{
    Report report;
    report.graph = graph;
    normalizeGraph(report.graph);
    const int fileCount = snapshot.files.size();
    const int moduleCount = report.graph.nodes.size();
    if (report.graph.moduleOfFile.size() != fileCount) {
        report.graph.moduleOfFile.fill(-1, fileCount);
        for (int m = 0; m < moduleCount; ++m) {
            for (int f : report.graph.nodes[m].files) {
                if (f >= 0 && f < fileCount)
                    report.graph.moduleOfFile[f] = m;
            }
        }
    }
    report.rules = evaluateRules(snapshot, report.graph);
    report.files.resize(fileCount);
    report.modules.resize(moduleCount);
    report.fileViolations.resize(fileCount);
    report.moduleViolations.resize(moduleCount);

    double years = snapshot.historyDays > 0 ? snapshot.historyDays / 365.0 : 1.0;
    bool history = false;
    for (const FileInfo &file : snapshot.files) {
        if (file.commits > 0) {
            history = true;
            break;
        }
    }
    report.history = history;
    const auto interestFor = [years](double debt, int commits) {
        if (commits <= 0 || debt <= 0)
            return 0.0;
        const double perYear = commits / std::max(0.1, years);
        return std::min(2.0 * debt, debt * 0.05 * perYear);
    };
    const auto mark = [](Score &score, const Violation &v) {
        score.debt += v.debt;
        ++score.violations;
        const int s = severityIndex(v.severity);
        if (score.worst < 0 || s < score.worst)
            score.worst = s;
    };

    for (int r = 0; r < report.rules.size(); ++r) {
        const Rule &rule = report.rules[r];
        for (int i = 0; i < rule.violations.size(); ++i) {
            const Violation &v = rule.violations[i];
            report.bySeverity[severityIndex(v.severity)] += 1;
            if (rule.moduleScope || v.file < 0) {
                if (v.module >= 0 && v.module < moduleCount) {
                    report.moduleViolations[v.module].append({r, i});
                    mark(report.modules[v.module], v);
                }
            } else if (v.file < fileCount) {
                report.fileViolations[v.file].append({r, i});
                mark(report.files[v.file], v);
            }
            report.total.debt += v.debt;
            ++report.total.violations;
            const int s = severityIndex(v.severity);
            if (report.total.worst < 0 || s < report.total.worst)
                report.total.worst = s;
        }
    }

    Metrics &m = report.metrics;
    double complexitySum = 0;
    int duplicated = 0;
    for (int f = 0; f < fileCount; ++f) {
        const FileInfo &file = snapshot.files[f];
        Score &score = report.files[f];
        score.effort = effortMinutes(file.codeLines);
        score.ratio = score.effort > 0 ? score.debt / score.effort : (score.debt > 0 ? 1.0 : 0.0);
        score.rating = ratingFor(score.ratio);
        score.interest = interestFor(score.debt, file.commits);
        report.total.interest += score.interest;
        m.codeLines += file.codeLines;
        m.commentLines += file.commentLines;
        m.types += file.types;
        m.functions += file.functions.size();
        duplicated += file.duplicatedLines;
        if (file.test)
            ++m.testFiles;
        for (const FunctionInfo &fn : file.functions) {
            complexitySum += fn.complexity;
            if (fn.complexity > m.maxComplexity) {
                m.maxComplexity = fn.complexity;
                m.maxComplexityFunction = fn.name;
                m.maxComplexityFile = f;
            }
        }
    }
    for (int i = 0; i < moduleCount; ++i) {
        const ModuleNode &node = report.graph.nodes[i];
        Score &score = report.modules[i];
        const double ownDebt = score.debt;
        int codeLines = 0;
        int commits = 0;
        for (int f : node.files) {
            if (f < 0 || f >= fileCount)
                continue;
            const Score &fileScore = report.files[f];
            score.debt += fileScore.debt;
            score.violations += fileScore.violations;
            score.interest += fileScore.interest;
            if (fileScore.worst >= 0 && (score.worst < 0 || fileScore.worst < score.worst))
                score.worst = fileScore.worst;
            codeLines += snapshot.files[f].codeLines;
            commits = std::max(commits, snapshot.files[f].commits);
        }
        if (node.codeLines > 0)
            codeLines = node.codeLines;
        const double ownInterest = interestFor(ownDebt, node.commits > 0 ? node.commits : commits);
        score.interest += ownInterest;
        report.total.interest += ownInterest;
        score.effort = effortMinutes(codeLines);
        score.ratio = score.effort > 0 ? score.debt / score.effort : (score.debt > 0 ? 1.0 : 0.0);
        score.rating = ratingFor(score.ratio);
    }
    m.files = fileCount;
    m.modules = moduleCount;
    m.averageComplexity = m.functions ? complexitySum / m.functions : 0;
    m.commentPercent = (m.codeLines + m.commentLines) > 0
                           ? 100.0 * m.commentLines / (m.codeLines + m.commentLines)
                           : 0;
    m.duplicatedPercent = m.codeLines > 0 ? std::min(100.0, 100.0 * duplicated / m.codeLines) : 0;
    report.total.effort = effortMinutes(m.codeLines);
    report.total.ratio = report.total.effort > 0 ? report.total.debt / report.total.effort : 0;
    report.total.rating = ratingFor(report.total.ratio);
    report.gates = evaluateGates(snapshot, report);
    return report;
}

QString summaryLine(const Report &report)
{
    QStringList parts;
    parts << QStringLiteral("Rating %1").arg(ratingLetter(report.total.rating));
    parts << QStringLiteral("%1 debt").arg(formatDebt(report.total.debt));
    const int failing = report.gatesWith(GateStatus::Fail);
    const int warning = report.gatesWith(GateStatus::Warn);
    if (failing)
        parts << QStringLiteral("%1 %2 failing").arg(failing).arg(failing == 1 ? QStringLiteral("gate") : QStringLiteral("gates"));
    else if (warning)
        parts << QStringLiteral("%1 %2 warning").arg(warning).arg(warning == 1 ? QStringLiteral("gate") : QStringLiteral("gates"));
    else
        parts << QStringLiteral("all gates pass");
    return parts.join(QStringLiteral(" · "));
}

namespace {

void isotonic(QVector<double> &values, const QVector<double> &weights)
{
    struct Block {
        double sum;
        double weight;
        int count;
    };
    QVector<Block> blocks;
    blocks.reserve(values.size());
    for (int i = 0; i < values.size(); ++i) {
        const double w = std::max(weights[i], 1e-3);
        blocks.append({values[i] * w, w, 1});
        while (blocks.size() >= 2) {
            const Block &b = blocks.last();
            const Block &a = blocks[blocks.size() - 2];
            if (a.sum / a.weight <= b.sum / b.weight)
                break;
            Block merged{a.sum + b.sum, a.weight + b.weight, a.count + b.count};
            blocks.removeLast();
            blocks.last() = merged;
        }
    }
    int i = 0;
    for (const Block &b : blocks) {
        const double mean = b.sum / b.weight;
        for (int k = 0; k < b.count; ++k)
            values[i++] = mean;
    }
}

}

GraphLayout layeredLayout(const LayoutInput &input)
{
    GraphLayout out;
    const int n = input.sizes.size();
    out.boxes.resize(n);
    out.ranks.fill(-1, n);
    if (n == 0)
        return out;

    QList<LayoutLink> links;
    QSet<qint64> seen;
    QList<QVector<int>> adjacency(n);
    QVector<int> degree(n, 0);
    for (const LayoutLink &link : input.links) {
        if (link.from < 0 || link.from >= n || link.to < 0 || link.to >= n || link.from == link.to)
            continue;
        const qint64 key = qint64(link.from) * n + link.to;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        links.append(link);
        adjacency[link.from].append(link.to);
        ++degree[link.from];
        ++degree[link.to];
    }
    const QList<QVector<int>> parts = components(adjacency);
    QVector<int> comp(n, -1);
    QVector<int> compSize(parts.size(), 0);
    for (int c = 0; c < parts.size(); ++c) {
        compSize[c] = parts[c].size();
        for (int v : parts[c])
            comp[v] = c;
    }
    QVector<int> layer(n, 0);
    if (input.layers.size() == n) {
        for (int i = 0; i < n; ++i)
            layer[i] = std::max(0, input.layers[i]);
    } else {
        QVector<int> compLayer(parts.size(), 0);
        for (int c = 0; c < parts.size(); ++c) {
            for (int v : parts[c]) {
                for (int w : adjacency[v]) {
                    if (comp[w] != c)
                        compLayer[c] = std::max(compLayer[c], compLayer[comp[w]] + 1);
                }
            }
        }
        for (int i = 0; i < n; ++i)
            layer[i] = compLayer[comp[i]];
    }

    QVector<int> active;
    QVector<int> isolated;
    for (int i = 0; i < n; ++i)
        (degree[i] > 0 ? active : isolated).append(i);

    QVector<int> sub(n, 0);
    {
        QHash<qint64, int> weightOf;
        for (const LayoutLink &link : links)
            weightOf.insert(qint64(link.from) * n + link.to, std::max(1, link.weight));
        for (const QVector<int> &part : parts) {
            if (part.size() < 2)
                continue;
            const QSet<int> members(part.begin(), part.end());
            QSet<int> remaining = members;
            QHash<int, double> outWeight;
            QHash<int, double> inWeight;
            QList<QVector<int>> inside(n);
            for (int v : part) {
                for (int w : adjacency[v]) {
                    if (!members.contains(w))
                        continue;
                    const double weight = weightOf.value(qint64(v) * n + w, 1);
                    outWeight[v] += weight;
                    inWeight[w] += weight;
                    inside[v].append(w);
                }
            }
            QList<QVector<int>> incoming(n);
            for (int v : part) {
                for (int w : inside[v])
                    incoming[w].append(v);
            }
            const auto remove = [&](int v) {
                remaining.remove(v);
                for (int w : inside[v]) {
                    if (remaining.contains(w))
                        inWeight[w] -= weightOf.value(qint64(v) * n + w, 1);
                }
                for (int u : incoming[v]) {
                    if (remaining.contains(u))
                        outWeight[u] -= weightOf.value(qint64(u) * n + v, 1);
                }
            };
            QVector<int> front;
            QVector<int> back;
            while (!remaining.isEmpty()) {
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (int v : part) {
                        if (remaining.contains(v) && outWeight.value(v) <= 1e-9) {
                            back.prepend(v);
                            remove(v);
                            changed = true;
                        }
                    }
                    for (int v : part) {
                        if (remaining.contains(v) && inWeight.value(v) <= 1e-9) {
                            front.append(v);
                            remove(v);
                            changed = true;
                        }
                    }
                }
                if (remaining.isEmpty())
                    break;
                int best = -1;
                double bestDelta = -1e18;
                for (int v : part) {
                    if (!remaining.contains(v))
                        continue;
                    const double delta = outWeight.value(v) - inWeight.value(v);
                    if (delta > bestDelta) {
                        bestDelta = delta;
                        best = v;
                    }
                }
                front.append(best);
                remove(best);
            }
            const QVector<int> sequence = front + back;
            QHash<int, int> position;
            for (int i = 0; i < sequence.size(); ++i)
                position.insert(sequence[i], i);
            for (int v : sequence) {
                for (int u : incoming[v]) {
                    if (position.value(u) < position.value(v))
                        sub[v] = std::max(sub[v], sub[u] + 1);
                }
            }
        }
    }

    QVector<int> rank(n, -1);
    {
        int maxLayer = 0;
        int maxSub = 0;
        for (int v : active) {
            maxLayer = std::max(maxLayer, layer[v]);
            maxSub = std::max(maxSub, sub[v]);
        }
        const qint64 band = maxSub + 1;
        QVector<qint64> distinct;
        for (int v : active)
            distinct.append(qint64(maxLayer - layer[v]) * band + sub[v]);
        std::sort(distinct.begin(), distinct.end());
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        QHash<qint64, int> rankOf;
        for (int i = 0; i < distinct.size(); ++i)
            rankOf.insert(distinct[i], i);
        for (int v : active)
            rank[v] = rankOf.value(qint64(maxLayer - layer[v]) * band + sub[v]);
    }
    int rankCount = 0;
    for (int v : active)
        rankCount = std::max(rankCount, rank[v] + 1);

    int cap = input.maxPerRank;
    if (cap <= 0) {
        double widths = 0;
        double heights = 0;
        for (int v : active) {
            widths += input.sizes[v].width() + input.nodeGap;
            heights += input.sizes[v].height() + input.layerGap;
        }
        const double count = std::max(1, int(active.size()));
        const double aspect = 1.5;
        cap = std::max(4, int(std::lround(std::sqrt(aspect * count * (heights / count) / std::max(1.0, widths / count)))));
    }
    QList<QVector<int>> predecessors(n);
    for (const LayoutLink &link : links)
        predecessors[link.to].append(link.from);
    {
        const int maxRank = rankCount - 1;
        QVector<int> sequence = active;
        std::stable_sort(sequence.begin(), sequence.end(), [&](int a, int b) {
            const int la = maxRank - rank[a];
            const int lb = maxRank - rank[b];
            if (la != lb)
                return la < lb;
            if (predecessors[a].size() != predecessors[b].size())
                return predecessors[a].size() > predecessors[b].size();
            return a < b;
        });
        QVector<int> level(n, -1);
        QHash<int, int> fill;
        int top = 0;
        for (int v : sequence) {
            int l = maxRank - rank[v];
            for (int w : adjacency[v]) {
                if (rank[w] > rank[v] && level[w] >= 0)
                    l = std::max(l, level[w] + 1);
            }
            while (fill.value(l) >= cap)
                ++l;
            level[v] = l;
            fill[l] += 1;
            top = std::max(top, l);
        }
        for (int v : active)
            rank[v] = top - level[v];
        rankCount = top + 1;
    }
    {
        QVector<int> used(rankCount, 0);
        for (int v : active)
            used[rank[v]] = 1;
        QVector<int> remap(rankCount, -1);
        int next = 0;
        for (int r = 0; r < rankCount; ++r) {
            if (used[r])
                remap[r] = next++;
        }
        for (int v : active)
            rank[v] = remap[rank[v]];
        rankCount = next;
    }

    struct Chain {
        int link = -1;
        bool reversed = false;
        bool flat = false;
        QVector<int> nodes;
    };
    QVector<int> vrank = rank;
    QVector<double> vwidth(n);
    for (int i = 0; i < n; ++i)
        vwidth[i] = input.sizes[i].width();
    const double dummyWidth = 6;
    QList<QVector<int>> down(n);
    QList<QVector<int>> up(n);
    QList<Chain> chains;
    int dummies = 0;
    for (int l = 0; l < links.size(); ++l) {
        const LayoutLink &link = links[l];
        Chain chain;
        chain.link = l;
        const int ru = rank[link.from];
        const int rv = rank[link.to];
        if (ru == rv) {
            chain.flat = true;
            chain.nodes = {link.from, link.to};
            chains.append(chain);
            continue;
        }
        chain.reversed = ru > rv;
        const int top = chain.reversed ? link.to : link.from;
        const int bottom = chain.reversed ? link.from : link.to;
        chain.nodes.append(top);
        const int span = rank[bottom] - rank[top] - 1;
        if (dummies + span <= 30000) {
            for (int r = rank[top] + 1; r < rank[bottom]; ++r) {
                const int d = vrank.size();
                vrank.append(r);
                vwidth.append(dummyWidth);
                down.append(QVector<int>());
                up.append(QVector<int>());
                chain.nodes.append(d);
                ++dummies;
            }
        }
        chain.nodes.append(bottom);
        for (int k = 0; k + 1 < chain.nodes.size(); ++k) {
            down[chain.nodes[k]].append(chain.nodes[k + 1]);
            up[chain.nodes[k + 1]].append(chain.nodes[k]);
        }
        chains.append(chain);
    }
    const int total = vrank.size();
    const auto isReal = [n](int v) { return v < n; };

    QVector<QVector<int>> order(rankCount);
    for (int v = 0; v < total; ++v) {
        if (vrank[v] >= 0)
            order[vrank[v]].append(v);
    }
    QVector<double> pos(total, 0);
    const auto refreshPositions = [&]() {
        for (const QVector<int> &row : order) {
            for (int i = 0; i < row.size(); ++i)
                pos[row[i]] = i;
        }
    };
    const auto groupOf = [&](int v) { return isReal(v) && compSize[comp[v]] > 1 ? comp[v] : -1; };
    if (!order.isEmpty()) {
        std::stable_sort(order[0].begin(), order[0].end(), [&](int a, int b) {
            return std::make_pair(comp[a], a) < std::make_pair(comp[b], b);
        });
    }
    refreshPositions();
    const auto sweep = [&](int r, bool fromAbove) {
        QVector<int> &row = order[r];
        const int count = row.size();
        QVector<double> own(count);
        for (int i = 0; i < count; ++i) {
            const QVector<int> &neighbours = fromAbove ? up[row[i]] : down[row[i]];
            if (neighbours.isEmpty()) {
                own[i] = pos[row[i]];
                continue;
            }
            double sum = 0;
            for (int w : neighbours)
                sum += pos[w];
            own[i] = sum / neighbours.size();
        }
        QHash<int, QPair<double, int>> unit;
        for (int i = 0; i < count; ++i) {
            const int g = groupOf(row[i]);
            if (g < 0)
                continue;
            auto &entry = unit[g];
            entry.first += own[i];
            entry.second += 1;
        }
        struct Key {
            double unit;
            int tag;
            double own;
            double current;
            int v;
        };
        QVector<Key> keys(count);
        for (int i = 0; i < count; ++i) {
            const int v = row[i];
            const int g = groupOf(v);
            double unitKey = own[i];
            if (g >= 0) {
                const auto entry = unit.value(g);
                unitKey = entry.first / entry.second;
            }
            keys[i] = {unitKey, g >= 0 ? g : n + v, own[i], pos[v], v};
        }
        std::stable_sort(keys.begin(), keys.end(), [](const Key &a, const Key &b) {
            if (a.unit != b.unit)
                return a.unit < b.unit;
            if (a.tag != b.tag)
                return a.tag < b.tag;
            if (a.own != b.own)
                return a.own < b.own;
            return a.current < b.current;
        });
        for (int i = 0; i < count; ++i) {
            row[i] = keys[i].v;
            pos[row[i]] = i;
        }
    };
    const auto countCrossings = [&]() {
        int crossings = 0;
        for (int r = 0; r + 1 < rankCount; ++r) {
            QVector<QPair<int, int>> edges;
            for (int v : order[r]) {
                for (int w : down[v]) {
                    if (vrank[w] == r + 1)
                        edges.append({int(pos[v]), int(pos[w])});
                }
            }
            std::sort(edges.begin(), edges.end());
            const int width = order[r + 1].size() + 1;
            QVector<int> tree(width + 1, 0);
            int seenCount = 0;
            for (const auto &edge : edges) {
                int greater = seenCount;
                for (int i = edge.second + 1; i > 0; i -= i & -i)
                    greater -= tree[i];
                crossings += greater;
                for (int i = edge.second + 1; i <= width; i += i & -i)
                    tree[i] += 1;
                ++seenCount;
            }
        }
        return crossings;
    };
    for (int r = 1; r < rankCount; ++r)
        sweep(r, true);
    QVector<QVector<int>> best = order;
    int bestCrossings = countCrossings();
    for (int iteration = 0; iteration < 16 && bestCrossings > 0; ++iteration) {
        if (iteration % 2 == 0) {
            for (int r = 1; r < rankCount; ++r)
                sweep(r, true);
        } else {
            for (int r = rankCount - 2; r >= 0; --r)
                sweep(r, false);
        }
        const int crossings = countCrossings();
        if (crossings < bestCrossings) {
            bestCrossings = crossings;
            best = order;
        }
    }
    order = best;
    refreshPositions();
    out.crossings = bestCrossings;

    QVector<double> x(total, 0);
    const auto separation = [&](int a, int b) {
        const bool ra = isReal(a);
        const bool rb = isReal(b);
        const double gap = ra && rb ? input.nodeGap : (ra || rb ? input.nodeGap * 0.45 : 10.0);
        return (vwidth[a] + vwidth[b]) / 2.0 + gap;
    };
    for (const QVector<int> &row : order) {
        double cursor = 0;
        for (int i = 0; i < row.size(); ++i) {
            if (i > 0)
                cursor += separation(row[i - 1], row[i]);
            x[row[i]] = cursor;
        }
    }
    const auto place = [&](int r, int mode) {
        const QVector<int> &row = order[r];
        const int count = row.size();
        if (count == 0)
            return;
        QVector<double> offsets(count, 0);
        for (int i = 1; i < count; ++i)
            offsets[i] = offsets[i - 1] + separation(row[i - 1], row[i]);
        QVector<double> targets(count);
        QVector<double> weights(count);
        for (int i = 0; i < count; ++i) {
            const int v = row[i];
            double sum = 0;
            int k = 0;
            if (mode != 1) {
                for (int w : up[v]) {
                    sum += x[w];
                    ++k;
                }
            }
            if (mode != 0) {
                for (int w : down[v]) {
                    sum += x[w];
                    ++k;
                }
            }
            const double desired = k ? sum / k : x[v];
            targets[i] = desired - offsets[i];
            weights[i] = k == 0 ? 0.05 : (isReal(v) ? 1.0 + 0.2 * k : 6.0);
        }
        isotonic(targets, weights);
        for (int i = 0; i < count; ++i)
            x[row[i]] = targets[i] + offsets[i];
    };
    for (int iteration = 0; iteration < 10; ++iteration) {
        for (int r = 1; r < rankCount; ++r)
            place(r, 0);
        for (int r = rankCount - 2; r >= 0; --r)
            place(r, 1);
    }
    for (int r = 0; r < rankCount; ++r)
        place(r, 2);

    QVector<double> rankTop(rankCount, 0);
    QVector<double> rankHeight(rankCount, 20);
    for (int v : active)
        rankHeight[rank[v]] = std::max(rankHeight[rank[v]], input.sizes[v].height());
    double cursorY = 0;
    for (int r = 0; r < rankCount; ++r) {
        rankTop[r] = cursorY;
        cursorY += rankHeight[r] + input.layerGap;
    }
    double minX = std::numeric_limits<double>::max();
    for (int v = 0; v < total; ++v) {
        if (vrank[v] >= 0)
            minX = std::min(minX, x[v] - vwidth[v] / 2.0);
    }
    if (minX == std::numeric_limits<double>::max())
        minX = 0;
    for (int v = 0; v < total; ++v)
        x[v] -= minX;
    for (int v : active) {
        const QSizeF size = input.sizes[v];
        const int r = rank[v];
        out.boxes[v] = QRectF(x[v] - size.width() / 2.0, rankTop[r] + (rankHeight[r] - size.height()) / 2.0,
                              size.width(), size.height());
        out.ranks[v] = r;
    }

    QHash<qint64, double> exitPort;
    QHash<qint64, double> entryPort;
    {
        QList<QVector<QPair<double, int>>> exits(n);
        QList<QVector<QPair<double, int>>> entries(n);
        for (int c = 0; c < chains.size(); ++c) {
            const Chain &chain = chains[c];
            if (chain.flat)
                continue;
            const int top = chain.nodes.first();
            const int bottom = chain.nodes.last();
            exits[top].append({x[chain.nodes[1]], c});
            entries[bottom].append({x[chain.nodes[chain.nodes.size() - 2]], c});
        }
        for (int v = 0; v < n; ++v) {
            const QRectF box = out.boxes[v];
            const auto assign = [&](QVector<QPair<double, int>> &list, QHash<qint64, double> &ports) {
                std::stable_sort(list.begin(), list.end());
                const int m = list.size();
                for (int k = 0; k < m; ++k) {
                    const double t = m == 1 ? 0.5 : 0.22 + 0.56 * double(k) / (m - 1);
                    ports.insert(qint64(list[k].second) * n + v, box.left() + box.width() * t);
                }
            };
            assign(exits[v], exitPort);
            assign(entries[v], entryPort);
        }
    }
    for (int c = 0; c < chains.size(); ++c) {
        const Chain &chain = chains[c];
        const LayoutLink &link = links[chain.link];
        RoutedEdge edge;
        edge.from = link.from;
        edge.to = link.to;
        edge.weight = link.weight;
        edge.cycle = link.cycle;
        edge.flat = chain.flat;
        if (chain.flat) {
            const QRectF a = out.boxes[link.from];
            const QRectF b = out.boxes[link.to];
            const bool below = a.center().x() <= b.center().x();
            const double dx = std::abs(b.center().x() - a.center().x());
            const double sx = a.center().x() + (below ? 0.18 : -0.18) * a.width();
            const double ex = b.center().x() + (below ? -0.18 : 0.18) * b.width();
            const double sy = below ? a.bottom() : a.top();
            const double ey = below ? b.bottom() : b.top();
            const double bulge = 26 + 0.12 * dx;
            const double my = below ? std::max(sy, ey) + bulge : std::min(sy, ey) - bulge;
            edge.points = {QPointF(sx, sy), QPointF((sx + ex) / 2.0, my), QPointF(ex, ey)};
        } else {
            const int top = chain.nodes.first();
            const int bottom = chain.nodes.last();
            QVector<QPointF> points;
            points.append(QPointF(exitPort.value(qint64(c) * n + top,
                                                 out.boxes[top].center().x()),
                                  out.boxes[top].bottom()));
            for (int k = 1; k + 1 < chain.nodes.size(); ++k) {
                const int d = chain.nodes[k];
                const int r = vrank[d];
                points.append(QPointF(x[d], rankTop[r]));
                points.append(QPointF(x[d], rankTop[r] + rankHeight[r]));
            }
            points.append(QPointF(entryPort.value(qint64(c) * n + bottom,
                                                  out.boxes[bottom].center().x()),
                                  out.boxes[bottom].top()));
            if (chain.reversed)
                std::reverse(points.begin(), points.end());
            edge.points = points;
        }
        out.edges.append(edge);
    }

    QRectF bounds;
    for (int v : active)
        bounds = bounds.united(out.boxes[v]);
    for (const RoutedEdge &edge : out.edges) {
        for (const QPointF &p : edge.points)
            bounds = bounds.united(QRectF(p, QSizeF(1, 1)));
    }
    if (!isolated.isEmpty()) {
        double area = 0;
        for (int v : isolated)
            area += (input.sizes[v].width() + input.nodeGap * 0.6) * (input.sizes[v].height() + input.nodeGap * 0.6);
        const bool beside = !active.isEmpty() && bounds.height() > bounds.width() * 1.1;
        if (beside) {
            const double startX = bounds.right() + input.nodeGap * 2.0;
            const double limit = std::max(bounds.height(), 400.0);
            double cx = startX;
            double cy = bounds.top();
            double columnWidth = 0;
            for (int v : isolated) {
                const QSizeF size = input.sizes[v];
                if (cy > bounds.top() && cy + size.height() > bounds.top() + limit) {
                    cy = bounds.top();
                    cx += columnWidth + input.nodeGap * 0.6;
                    columnWidth = 0;
                }
                out.boxes[v] = QRectF(QPointF(cx, cy), size);
                out.ranks[v] = rankCount;
                cy += size.height() + input.nodeGap * 0.6;
                columnWidth = std::max(columnWidth, size.width());
            }
        } else {
            const double startY = active.isEmpty() ? 0 : bounds.bottom() + input.layerGap * 0.7;
            const double limit = std::clamp(std::sqrt(area) * 1.9, 600.0, std::max(active.isEmpty() ? 0.0 : bounds.width(), 1100.0));
            double cx = active.isEmpty() ? 0 : bounds.left();
            const double left = cx;
            double cy = startY;
            double rowHeight = 0;
            for (int v : isolated) {
                const QSizeF size = input.sizes[v];
                if (cx > left && cx + size.width() > left + limit) {
                    cx = left;
                    cy += rowHeight + input.nodeGap * 0.6;
                    rowHeight = 0;
                }
                out.boxes[v] = QRectF(QPointF(cx, cy), size);
                out.ranks[v] = rankCount;
                cx += size.width() + input.nodeGap * 0.6;
                rowHeight = std::max(rowHeight, size.height());
            }
        }
        for (int v : isolated)
            bounds = bounds.united(out.boxes[v]);
        ++rankCount;
    }
    out.bounds = bounds;
    out.rankCount = rankCount;
    return out;
}

QSizeF moduleBoxSize(int codeLines)
{
    const double width = std::clamp(96.0 + std::sqrt(std::max(0, codeLines)) * 1.0, 128.0, 340.0);
    const double height = std::clamp(width * 0.62, 76.0, 210.0);
    return QSizeF(std::round(width), std::round(height));
}

QVector<int> moduleLayers(const ModuleGraph &graph)
{
    const int n = graph.nodes.size();
    QVector<int> layers(n, 0);
    bool hinted = false;
    for (const ModuleNode &node : graph.nodes) {
        if (node.layer > 0) {
            hinted = true;
            break;
        }
    }
    if (hinted) {
        for (int i = 0; i < n; ++i)
            layers[i] = graph.nodes[i].layer;
        bool consistent = true;
        for (const ModuleEdge &edge : graph.edges) {
            if (!edge.inCycle && edge.from >= 0 && edge.to >= 0 && layers[edge.from] <= layers[edge.to]) {
                consistent = false;
                break;
            }
        }
        if (consistent)
            return layers;
    }
    QList<QVector<int>> adjacency(n);
    for (const ModuleEdge &edge : graph.edges) {
        if (edge.from >= 0 && edge.from < n && edge.to >= 0 && edge.to < n && edge.from != edge.to)
            adjacency[edge.from].append(edge.to);
    }
    const QList<QVector<int>> parts = components(adjacency);
    QVector<int> comp(n, 0);
    for (int c = 0; c < parts.size(); ++c) {
        for (int v : parts[c])
            comp[v] = c;
    }
    QVector<int> compLayer(parts.size(), 0);
    for (int c = 0; c < parts.size(); ++c) {
        for (int v : parts[c]) {
            for (int w : adjacency[v]) {
                if (comp[w] != c)
                    compLayer[c] = std::max(compLayer[c], compLayer[comp[w]] + 1);
            }
        }
    }
    for (int i = 0; i < n; ++i)
        layers[i] = compLayer[comp[i]];
    return layers;
}

GraphLayout moduleLayout(const ModuleGraph &graph)
{
    LayoutInput input;
    for (const ModuleNode &node : graph.nodes)
        input.sizes.append(moduleBoxSize(node.codeLines));
    input.layers = moduleLayers(graph);
    for (const ModuleEdge &edge : graph.edges)
        input.links.append({edge.from, edge.to, edge.weight, edge.inCycle});
    input.layerGap = 80;
    input.nodeGap = 44;
    return layeredLayout(input);
}

DrillGraph drillGraph(const Snapshot &snapshot, const ModuleGraph &graph, int module, int maxFiles)
{
    DrillGraph out;
    out.module = module;
    if (module < 0 || module >= graph.nodes.size())
        return out;
    QVector<int> files = graph.nodes[module].files;
    std::stable_sort(files.begin(), files.end(), [&snapshot](int a, int b) {
        const FileInfo &fa = snapshot.files[a];
        const FileInfo &fb = snapshot.files[b];
        const int da = fa.dependsOn.size() + fa.dependedOnBy.size();
        const int db = fb.dependsOn.size() + fb.dependedOnBy.size();
        if ((da > 0) != (db > 0))
            return da > 0;
        return fa.codeLines + 40 * da > fb.codeLines + 40 * db;
    });
    if (files.size() > maxFiles) {
        out.hiddenFiles = files.size() - maxFiles;
        files.resize(maxFiles);
    }
    QHash<int, int> nodeOfFile;
    for (int f : files) {
        nodeOfFile.insert(f, out.nodes.size());
        out.nodes.append({false, false, f, snapshot.files[f].codeLines});
    }
    const auto moduleOf = [&graph](int file) {
        return file >= 0 && file < graph.moduleOfFile.size() ? graph.moduleOfFile[file] : -1;
    };
    QHash<int, int> usesWeight;
    QHash<int, int> usedByWeight;
    QHash<qint64, int> fileToModule;
    QHash<qint64, int> moduleToFile;
    QList<LayoutLink> internal;
    for (int f : files) {
        const FileInfo &file = snapshot.files[f];
        for (int d : file.dependsOn) {
            const int m = moduleOf(d);
            if (m == module) {
                if (nodeOfFile.contains(d) && d != f)
                    internal.append({nodeOfFile.value(f), nodeOfFile.value(d), 1, false});
            } else if (m >= 0) {
                usesWeight[m] += 1;
                fileToModule[qint64(f) * graph.nodes.size() + m] += 1;
            }
        }
        for (int s : file.dependedOnBy) {
            const int m = moduleOf(s);
            if (m >= 0 && m != module) {
                usedByWeight[m] += 1;
                moduleToFile[qint64(f) * graph.nodes.size() + m] += 1;
            }
        }
    }
    const auto strongest = [](const QHash<int, int> &weights, int limit) {
        QVector<QPair<int, int>> list;
        for (auto it = weights.constBegin(); it != weights.constEnd(); ++it)
            list.append({-it.value(), it.key()});
        std::sort(list.begin(), list.end());
        QVector<int> keep;
        for (int i = 0; i < list.size() && i < limit; ++i)
            keep.append(list[i].second);
        return keep;
    };
    QHash<int, int> usesNode;
    for (int m : strongest(usesWeight, 24)) {
        usesNode.insert(m, out.nodes.size());
        out.nodes.append({true, false, m, usesWeight.value(m)});
    }
    QHash<int, int> usedByNode;
    for (int m : strongest(usedByWeight, 24)) {
        usedByNode.insert(m, out.nodes.size());
        out.nodes.append({true, true, m, usedByWeight.value(m)});
    }
    out.links = internal;
    const int moduleCount = graph.nodes.size();
    for (auto it = fileToModule.constBegin(); it != fileToModule.constEnd(); ++it) {
        const int f = int(it.key() / moduleCount);
        const int m = int(it.key() % moduleCount);
        if (usesNode.contains(m))
            out.links.append({nodeOfFile.value(f), usesNode.value(m), it.value(), false});
    }
    for (auto it = moduleToFile.constBegin(); it != moduleToFile.constEnd(); ++it) {
        const int f = int(it.key() / moduleCount);
        const int m = int(it.key() % moduleCount);
        if (usedByNode.contains(m))
            out.links.append({usedByNode.value(m), nodeOfFile.value(f), it.value(), false});
    }
    std::sort(out.links.begin(), out.links.end(), [](const LayoutLink &a, const LayoutLink &b) {
        return std::make_pair(a.from, a.to) < std::make_pair(b.from, b.to);
    });
    {
        QList<QVector<int>> adjacency(out.nodes.size());
        for (const LayoutLink &link : out.links)
            adjacency[link.from].append(link.to);
        QSet<qint64> cyclic;
        for (const QVector<int> &part : components(adjacency)) {
            if (part.size() < 2)
                continue;
            const QSet<int> members(part.begin(), part.end());
            for (int v : part) {
                for (int w : adjacency[v]) {
                    if (members.contains(w))
                        cyclic.insert(qint64(v) * out.nodes.size() + w);
                }
            }
        }
        for (LayoutLink &link : out.links)
            link.cycle = cyclic.contains(qint64(link.from) * out.nodes.size() + link.to);
    }
    LayoutInput input;
    for (const DrillNode &node : out.nodes) {
        if (node.module) {
            const QString name = graph.nodes[node.index].name;
            input.sizes.append(QSizeF(std::clamp(60.0 + name.size() * 7.0, 120.0, 230.0), 36));
        } else {
            const QString name = fileName(snapshot.files[node.index].path);
            const double byName = 36.0 + name.size() * 7.0;
            const double bySize = 80.0 + std::sqrt(std::max(0, node.weight)) * 1.6;
            input.sizes.append(QSizeF(std::clamp(std::max(byName, bySize), 110.0, 260.0), 48));
        }
    }
    input.links = out.links;
    input.layerGap = 84;
    input.nodeGap = 28;
    out.layout = layeredLayout(input);
    return out;
}

QVector<int> matrixOrder(const ModuleGraph &graph)
{
    const int n = graph.nodes.size();
    const QVector<int> layers = moduleLayers(graph);
    QHash<int, int> cycleLayer;
    QHash<int, QString> cycleName;
    for (int i = 0; i < n; ++i) {
        const int c = graph.nodes[i].cycle;
        if (c < 0)
            continue;
        cycleLayer[c] = std::max(cycleLayer.value(c, 0), layers[i]);
        const QString current = cycleName.value(c);
        if (current.isEmpty() || graph.nodes[i].path < current)
            cycleName[c] = graph.nodes[i].path;
    }
    QVector<int> order(n);
    for (int i = 0; i < n; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const ModuleNode &na = graph.nodes[a];
        const ModuleNode &nb = graph.nodes[b];
        const int la = na.cycle >= 0 ? cycleLayer.value(na.cycle) : layers[a];
        const int lb = nb.cycle >= 0 ? cycleLayer.value(nb.cycle) : layers[b];
        if (la != lb)
            return la > lb;
        const QString ga = na.cycle >= 0 ? cycleName.value(na.cycle) : na.path;
        const QString gb = nb.cycle >= 0 ? cycleName.value(nb.cycle) : nb.path;
        if (ga != gb)
            return ga < gb;
        const int ca = na.cycle >= 0 ? 0 : 1;
        const int cb = nb.cycle >= 0 ? 0 : 1;
        if (ca != cb)
            return ca < cb;
        return na.path < nb.path;
    });
    return order;
}

QVector<QRectF> squarify(const QVector<double> &weights, const QRectF &rect)
{
    QVector<QRectF> out(weights.size());
    QVector<int> order;
    double total = 0;
    for (int i = 0; i < weights.size(); ++i) {
        if (weights[i] > 0) {
            order.append(i);
            total += weights[i];
        }
    }
    if (order.isEmpty() || total <= 0 || rect.width() <= 0 || rect.height() <= 0)
        return out;
    std::stable_sort(order.begin(), order.end(), [&weights](int a, int b) { return weights[a] > weights[b]; });
    const double scale = rect.width() * rect.height() / total;
    QRectF free = rect;
    int i = 0;
    while (i < order.size()) {
        const double side = std::min(free.width(), free.height());
        if (side <= 0) {
            for (; i < order.size(); ++i)
                out[order[i]] = QRectF(free.topLeft(), QSizeF(0, 0));
            break;
        }
        double rowSum = 0;
        double minArea = std::numeric_limits<double>::max();
        double maxArea = 0;
        double worst = std::numeric_limits<double>::max();
        int j = i;
        while (j < order.size()) {
            const double area = weights[order[j]] * scale;
            const double sum = rowSum + area;
            const double lo = std::min(minArea, area);
            const double hi = std::max(maxArea, area);
            const double s2 = side * side;
            const double candidate = std::max(s2 * hi / (sum * sum), (sum * sum) / (s2 * lo));
            if (j > i && candidate > worst)
                break;
            worst = candidate;
            rowSum = sum;
            minArea = lo;
            maxArea = hi;
            ++j;
        }
        const double thickness = rowSum / side;
        if (free.width() >= free.height()) {
            double y = free.top();
            for (int k = i; k < j; ++k) {
                const double h = weights[order[k]] * scale / thickness;
                out[order[k]] = QRectF(free.left(), y, thickness, k + 1 == j ? free.bottom() - y : h);
                y += h;
            }
            free.setLeft(std::min(free.right(), free.left() + thickness));
        } else {
            double x = free.left();
            for (int k = i; k < j; ++k) {
                const double w = weights[order[k]] * scale / thickness;
                out[order[k]] = QRectF(x, free.top(), k + 1 == j ? free.right() - x : w, thickness);
                x += w;
            }
            free.setTop(std::min(free.bottom(), free.top() + thickness));
        }
        i = j;
    }
    return out;
}

QString reportHtml(const Snapshot &snapshot, const Report &report, const QString &title)
{
    const QLocale locale;
    const QStringList ratingColors = {QStringLiteral("#3fb98f"), QStringLiteral("#8cc265"),
                                      QStringLiteral("#e3b341"), QStringLiteral("#f0883e"),
                                      QStringLiteral("#e5534b")};
    const QStringList severityColors = {QStringLiteral("#e5534b"), QStringLiteral("#f0883e"),
                                        QStringLiteral("#e3b341"), QStringLiteral("#8b949e")};
    const auto gateColor = [](GateStatus status) {
        return status == GateStatus::Fail ? QStringLiteral("#e5534b")
               : status == GateStatus::Warn ? QStringLiteral("#e3b341")
                                            : QStringLiteral("#3fb98f");
    };
    QString html;
    html += QStringLiteral("<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">");
    html += QStringLiteral("<title>Strata report · %1</title>").arg(esc(title));
    html += QStringLiteral(
        "<style>"
        "body{font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:32px 40px;}"
        "h1{font-size:26px;margin:0 0 4px}h2{font-size:18px;margin:36px 0 12px;border-bottom:1px solid #30363d;padding-bottom:6px}"
        ".muted{color:#8b949e}.row{display:flex;gap:16px;flex-wrap:wrap}"
        ".tile{background:#161b22;border-radius:10px;padding:14px 18px;min-width:140px}"
        ".tile b{display:block;font-size:22px;margin-top:4px}"
        ".rating{width:84px;height:84px;border-radius:16px;font-size:48px;font-weight:700;color:#0d1117;display:flex;align-items:center;justify-content:center}"
        "table{border-collapse:collapse;width:100%;font-size:13px}td,th{text-align:left;padding:6px 10px;border-bottom:1px solid #21262d;vertical-align:top}"
        "th{color:#8b949e;font-weight:600}.num{text-align:right;font-variant-numeric:tabular-nums}"
        ".dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px}"
        "details{background:#161b22;border-radius:8px;margin:6px 0;padding:8px 12px}summary{cursor:pointer}"
        "a{color:#58a6ff}"
        "</style></head><body>");
    html += QStringLiteral("<h1>Strata · %1</h1>").arg(esc(title));
    QStringList facts;
    if (!snapshot.branch.isEmpty())
        facts << QStringLiteral("branch %1").arg(esc(snapshot.branch));
    if (!snapshot.head.isEmpty())
        facts << QStringLiteral("commit %1").arg(esc(snapshot.head.left(10)));
    if (snapshot.analyzedAt > 0)
        facts << QStringLiteral("analyzed %1").arg(locale.toString(QDateTime::fromSecsSinceEpoch(snapshot.analyzedAt), QLocale::ShortFormat));
    html += QStringLiteral("<div class=\"muted\">%1</div>").arg(facts.join(QStringLiteral(" · ")));

    html += QStringLiteral("<h2 id=\"dashboard\">Dashboard</h2><div class=\"row\">");
    html += QStringLiteral("<div class=\"tile\" style=\"display:flex;gap:16px;align-items:center\"><div class=\"rating\" style=\"background:%1\">%2</div>"
                           "<div>Debt ratio<b>%3</b><span class=\"muted\">%4 debt · %5 interest per year</span></div></div>")
                .arg(ratingColors[report.total.rating], QString(ratingLetter(report.total.rating)),
                     formatPercent(report.total.ratio), formatDebt(report.total.debt),
                     formatDebt(report.total.interest));
    const Metrics &m = report.metrics;
    const QList<QPair<QString, QString>> tiles = {
        {QStringLiteral("Lines of code"), locale.toString(m.codeLines)},
        {QStringLiteral("Files"), locale.toString(m.files)},
        {QStringLiteral("Modules"), locale.toString(m.modules)},
        {QStringLiteral("Types"), locale.toString(m.types)},
        {QStringLiteral("Functions"), locale.toString(m.functions)},
        {QStringLiteral("Average complexity"), locale.toString(m.averageComplexity, 'f', 1)},
        {QStringLiteral("Max complexity"), locale.toString(m.maxComplexity)},
        {QStringLiteral("Comments"), QStringLiteral("%1 %").arg(locale.toString(m.commentPercent, 'f', 1))},
        {QStringLiteral("Duplicated"), QStringLiteral("%1 %").arg(locale.toString(m.duplicatedPercent, 'f', 1))},
    };
    for (const auto &tile : tiles)
        html += QStringLiteral("<div class=\"tile\">%1<b>%2</b></div>").arg(esc(tile.first), esc(tile.second));
    html += QStringLiteral("</div>");

    html += QStringLiteral("<h2 id=\"gates\">Quality gates</h2><table><tr><th>Gate</th><th>Status</th><th>Measured</th><th>Rule</th></tr>");
    for (const GateResult &gate : report.gates) {
        html += QStringLiteral("<tr><td>%1</td><td><span class=\"dot\" style=\"background:%2\"></span>%3</td><td>%4</td><td class=\"muted\">%5</td></tr>")
                    .arg(esc(gate.name), gateColor(gate.status), gateStatusName(gate.status), esc(gate.measured), esc(gate.detail));
    }
    html += QStringLiteral("</table>");

    html += QStringLiteral("<h2 id=\"rules\">Rules</h2>");
    QList<int> ruleOrder;
    for (int i = 0; i < report.rules.size(); ++i)
        ruleOrder.append(i);
    std::stable_sort(ruleOrder.begin(), ruleOrder.end(), [&report](int a, int b) {
        return report.rules[a].debt > report.rules[b].debt;
    });
    for (int r : ruleOrder) {
        const Rule &rule = report.rules[r];
        const int worst = rule.violations.isEmpty() ? severityIndex(rule.severity) : rule.worst();
        html += QStringLiteral("<details><summary><span class=\"dot\" style=\"background:%1\"></span><b>%2</b> %3 · %4 · %5 %6 · %7</summary>")
                    .arg(rule.violations.isEmpty() ? QStringLiteral("#3fb98f") : severityColors[worst], esc(rule.id), esc(rule.title),
                         categoryName(rule.category))
                    .arg(rule.violations.size())
                    .arg(rule.violations.size() == 1 ? QStringLiteral("violation") : QStringLiteral("violations"))
                    .arg(formatDebt(rule.debt));
        html += QStringLiteral("<p>%1</p><p class=\"muted\">How to fix: %2</p>").arg(esc(rule.description), esc(rule.howToFix));
        if (!rule.violations.isEmpty()) {
            html += QStringLiteral("<table><tr><th>Severity</th><th>Where</th><th>Issue</th><th class=\"num\">Debt</th></tr>");
            const int shown = std::min<int>(rule.violations.size(), 200);
            for (int i = 0; i < shown; ++i) {
                const Violation &v = rule.violations[i];
                QString where;
                if (v.file >= 0 && v.file < snapshot.files.size() && !rule.moduleScope)
                    where = snapshot.files[v.file].path + (v.line > 0 ? QStringLiteral(":%1").arg(v.line) : QString());
                else if (v.module >= 0 && v.module < report.graph.nodes.size())
                    where = report.graph.nodes[v.module].path;
                html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td class=\"num\">%4</td></tr>")
                            .arg(severityName(v.severity), esc(where), esc(v.message), formatDebt(v.debt));
            }
            if (rule.violations.size() > shown)
                html += QStringLiteral("<tr><td colspan=\"4\" class=\"muted\">and %1 more</td></tr>").arg(rule.violations.size() - shown);
            html += QStringLiteral("</table>");
        }
        html += QStringLiteral("</details>");
    }

    html += QStringLiteral("<h2 id=\"modules\">Modules</h2><table><tr><th>Module</th><th>Rating</th><th class=\"num\">Lines of code</th>"
                           "<th class=\"num\">Files</th><th class=\"num\">Ca</th><th class=\"num\">Ce</th><th class=\"num\">I</th>"
                           "<th class=\"num\">A</th><th class=\"num\">D</th><th class=\"num\">Debt</th></tr>");
    QList<int> modules;
    for (int i = 0; i < report.graph.nodes.size(); ++i)
        modules.append(i);
    std::stable_sort(modules.begin(), modules.end(), [&report](int a, int b) {
        return report.modules[a].debt > report.modules[b].debt;
    });
    for (int i : modules) {
        const ModuleNode &node = report.graph.nodes[i];
        const Score &score = report.modules[i];
        html += QStringLiteral("<tr><td>%1%2</td><td><span class=\"dot\" style=\"background:%3\"></span>%4</td><td class=\"num\">%5</td>"
                               "<td class=\"num\">%6</td><td class=\"num\">%7</td><td class=\"num\">%8</td><td class=\"num\">%9</td>")
                    .arg(esc(node.path.isEmpty() ? node.name : node.path),
                         node.cycle >= 0 ? QStringLiteral(" <span style=\"color:#e5534b\">(cycle)</span>") : QString(),
                         ratingColors[score.rating], QString(ratingLetter(score.rating)), locale.toString(node.codeLines))
                    .arg(node.files.size())
                    .arg(node.afferent)
                    .arg(node.efferent)
                    .arg(QString::number(node.instability, 'f', 2));
        html += QStringLiteral("<td class=\"num\">%1</td><td class=\"num\">%2</td><td class=\"num\">%3</td></tr>")
                    .arg(QString::number(node.abstractness, 'f', 2), QString::number(node.distance, 'f', 2), formatDebt(score.debt));
    }
    html += QStringLiteral("</table>");
    html += QStringLiteral("<p class=\"muted\">Debt ratio = estimated remediation time ÷ development effort (18 man-days per 1,000 lines of code). "
                           "Ratings: A ≤ 5 %, B ≤ 10 %, C ≤ 20 %, D ≤ 50 %, E above. Generated by Strata in ForkMesh.</p>");
    html += QStringLiteral("</body></html>\n");
    return html;
}

ModelPtr buildModel(const insight::SnapshotPtr &snapshot, int depth)
{
    auto model = std::make_shared<Model>();
    model->snapshot = snapshot;
    model->depth = depth;
    if (!snapshot)
        return model;
    const ModuleGraph graph = insight::buildModuleGraph(*snapshot, depth);
    model->report = buildReport(*snapshot, graph);
    model->layout = moduleLayout(model->report.graph);
    model->order = matrixOrder(model->report.graph);
    return model;
}

}
