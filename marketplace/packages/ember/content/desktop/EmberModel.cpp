#include "EmberModel.h"

#include <QHash>
#include <QLocale>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace forkmesh::ember {

using insight::FileInfo;
using insight::HealthBand;
using insight::Snapshot;

namespace {

struct ChainNode {
    PackCircle *circle = nullptr;
    ChainNode *next = nullptr;
    ChainNode *previous = nullptr;
};

void place(const PackCircle &b, const PackCircle &a, PackCircle &c)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double d2 = dx * dx + dy * dy;
    if (d2 > 0) {
        double a2 = a.r + c.r;
        a2 *= a2;
        double b2 = b.r + c.r;
        b2 *= b2;
        if (a2 > b2) {
            const double x = (d2 + b2 - a2) / (2 * d2);
            const double y = std::sqrt(std::max(0.0, b2 / d2 - x * x));
            c.x = b.x - x * dx - y * dy;
            c.y = b.y - x * dy + y * dx;
        } else {
            const double x = (d2 + a2 - b2) / (2 * d2);
            const double y = std::sqrt(std::max(0.0, a2 / d2 - x * x));
            c.x = a.x + x * dx - y * dy;
            c.y = a.y + x * dy + y * dx;
        }
    } else {
        c.x = a.x + c.r;
        c.y = a.y;
    }
}

bool intersects(const PackCircle &a, const PackCircle &b)
{
    const double dr = a.r + b.r - 1e-6;
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return dr > 0 && dr * dr > dx * dx + dy * dy;
}

double score(const ChainNode *node)
{
    const PackCircle &a = *node->circle;
    const PackCircle &b = *node->next->circle;
    const double ab = a.r + b.r;
    const double dx = (a.x * b.r + b.x * a.r) / ab;
    const double dy = (a.y * b.r + b.y * a.r) / ab;
    return dx * dx + dy * dy;
}

bool enclosesNot(const PackCircle &a, const PackCircle &b)
{
    const double dr = a.r - b.r;
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return dr < 0 || dr * dr < dx * dx + dy * dy;
}

bool enclosesWeak(const PackCircle &a, const PackCircle &b)
{
    const double dr = a.r - b.r + std::max({a.r, b.r, 1.0}) * 1e-9;
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return dr > 0 && dr * dr > dx * dx + dy * dy;
}

bool enclosesWeakAll(const PackCircle &a, const QVector<PackCircle> &basis)
{
    for (const PackCircle &b : basis) {
        if (!enclosesWeak(a, b))
            return false;
    }
    return true;
}

PackCircle encloseBasis2(const PackCircle &a, const PackCircle &b)
{
    const double x21 = b.x - a.x;
    const double y21 = b.y - a.y;
    const double r21 = b.r - a.r;
    const double l = std::sqrt(x21 * x21 + y21 * y21);
    if (l <= 0)
        return a.r >= b.r ? a : b;
    return {(a.x + b.x + x21 / l * r21) / 2, (a.y + b.y + y21 / l * r21) / 2, (l + a.r + b.r) / 2};
}

PackCircle encloseBasis3(const PackCircle &a, const PackCircle &b, const PackCircle &c)
{
    const double x1 = a.x, y1 = a.y, r1 = a.r;
    const double x2 = b.x, y2 = b.y, r2 = b.r;
    const double x3 = c.x, y3 = c.y, r3 = c.r;
    const double a2 = x1 - x2;
    const double a3 = x1 - x3;
    const double b2 = y1 - y2;
    const double b3 = y1 - y3;
    const double c2 = r2 - r1;
    const double c3 = r3 - r1;
    const double d1 = x1 * x1 + y1 * y1 - r1 * r1;
    const double d2 = d1 - x2 * x2 - y2 * y2 + r2 * r2;
    const double d3 = d1 - x3 * x3 - y3 * y3 + r3 * r3;
    const double ab = a3 * b2 - a2 * b3;
    const double xa = (b2 * d3 - b3 * d2) / (ab * 2) - x1;
    const double xb = (b3 * c2 - b2 * c3) / ab;
    const double ya = (a3 * d2 - a2 * d3) / (ab * 2) - y1;
    const double yb = (a2 * c3 - a3 * c2) / ab;
    const double qa = xb * xb + yb * yb - 1;
    const double qb = 2 * (r1 + xa * xb + ya * yb);
    const double qc = xa * xa + ya * ya - r1 * r1;
    const double r = -(std::abs(qa) > 1e-6 ? (qb + std::sqrt(std::max(0.0, qb * qb - 4 * qa * qc))) / (2 * qa)
                                            : qc / qb);
    return {x1 + xa + xb * r, y1 + ya + yb * r, r};
}

PackCircle encloseBasis(const QVector<PackCircle> &basis)
{
    if (basis.size() == 1)
        return basis.first();
    if (basis.size() == 2)
        return encloseBasis2(basis[0], basis[1]);
    return encloseBasis3(basis[0], basis[1], basis[2]);
}

bool extendBasis(const QVector<PackCircle> &basis, const PackCircle &p, QVector<PackCircle> &out)
{
    if (enclosesWeakAll(p, basis)) {
        out = {p};
        return true;
    }
    for (const PackCircle &b : basis) {
        if (enclosesNot(p, b) && enclosesWeakAll(encloseBasis2(b, p), basis)) {
            out = {b, p};
            return true;
        }
    }
    for (int i = 0; i + 1 < basis.size(); ++i) {
        for (int j = i + 1; j < basis.size(); ++j) {
            if (enclosesNot(encloseBasis2(basis[i], basis[j]), p)
                && enclosesNot(encloseBasis2(basis[i], p), basis[j])
                && enclosesNot(encloseBasis2(basis[j], p), basis[i])
                && enclosesWeakAll(encloseBasis3(basis[i], basis[j], p), basis)) {
                out = {basis[i], basis[j], p};
                return true;
            }
        }
    }
    return false;
}

PackCircle boundingFallback(const QVector<PackCircle> &circles)
{
    double cx = 0;
    double cy = 0;
    for (const PackCircle &c : circles) {
        cx += c.x;
        cy += c.y;
    }
    cx /= circles.size();
    cy /= circles.size();
    double r = 0;
    for (const PackCircle &c : circles)
        r = std::max(r, std::hypot(c.x - cx, c.y - cy) + c.r);
    return {cx, cy, r};
}

QColor hex(const char *value)
{
    return QColor(QString::fromLatin1(value));
}

int bandIndex(double health)
{
    const HealthBand band = insight::healthBand(health);
    return band == HealthBand::Healthy ? 0 : band == HealthBand::Problematic ? 1 : 2;
}

double logShare(double value, double max)
{
    if (value <= 0 || max <= 0)
        return 0;
    return std::clamp(std::log1p(value) / std::log1p(max), 0.0, 1.0);
}

struct BuildNode {
    QString name;
    QString path;
    int file = -1;
    QVector<int> children;
    QHash<QString, int> folders;
};

}

double packSiblings(QVector<PackCircle> &circles)
{
    const int n = circles.size();
    if (n == 0)
        return 0;
    PackCircle &first = circles[0];
    first.x = 0;
    first.y = 0;
    if (n == 1)
        return first.r;
    PackCircle &second = circles[1];
    first.x = -second.r;
    second.x = first.r;
    second.y = 0;
    if (n == 2) {
        const PackCircle e = encloseBasis2(first, second);
        for (PackCircle &c : circles) {
            c.x -= e.x;
            c.y -= e.y;
        }
        return e.r;
    }
    place(circles[1], circles[0], circles[2]);
    std::vector<ChainNode> nodes(n);
    for (int i = 0; i < n; ++i)
        nodes[i].circle = &circles[i];
    ChainNode *a = &nodes[0];
    ChainNode *b = &nodes[1];
    ChainNode *c = &nodes[2];
    a->next = c->previous = b;
    b->next = a->previous = c;
    c->next = b->previous = a;

    for (int i = 3; i < n; ++i) {
        place(*a->circle, *b->circle, circles[i]);
        c = &nodes[i];
        ChainNode *j = b->next;
        ChainNode *k = a->previous;
        double sj = b->circle->r;
        double sk = a->circle->r;
        bool retry = false;
        do {
            if (sj <= sk) {
                if (intersects(*j->circle, *c->circle)) {
                    b = j;
                    a->next = b;
                    b->previous = a;
                    retry = true;
                    break;
                }
                sj += j->circle->r;
                j = j->next;
            } else {
                if (intersects(*k->circle, *c->circle)) {
                    a = k;
                    a->next = b;
                    b->previous = a;
                    retry = true;
                    break;
                }
                sk += k->circle->r;
                k = k->previous;
            }
        } while (j != k->next);
        if (retry) {
            --i;
            continue;
        }
        c->previous = a;
        c->next = b;
        a->next = c;
        b->previous = c;
        b = c;
        double aa = score(a);
        while ((c = c->next) != b) {
            const double ca = score(c);
            if (ca < aa) {
                a = c;
                aa = ca;
            }
        }
        b = a->next;
    }

    QVector<PackCircle> front;
    front.append(*b->circle);
    for (ChainNode *it = b->next; it != b; it = it->next)
        front.append(*it->circle);
    const PackCircle e = encloseCircles(front);
    for (PackCircle &circle : circles) {
        circle.x -= e.x;
        circle.y -= e.y;
    }
    return e.r;
}

PackCircle encloseCircles(const QVector<PackCircle> &input)
{
    if (input.isEmpty())
        return {};
    QVector<PackCircle> circles = input;
    quint64 state = 1;
    const auto random = [&state] {
        state = (1664525ULL * state + 1013904223ULL) % 4294967296ULL;
        return double(state) / 4294967296.0;
    };
    for (int m = circles.size(); m > 0;) {
        const int i = int(random() * m--);
        std::swap(circles[m], circles[i]);
    }
    QVector<PackCircle> basis;
    PackCircle e;
    bool haveE = false;
    int guard = 0;
    const int limit = std::max(1000, int(circles.size()) * 64);
    for (int i = 0; i < circles.size();) {
        const PackCircle &p = circles[i];
        if (haveE && enclosesWeak(e, p)) {
            ++i;
            continue;
        }
        QVector<PackCircle> next;
        if (!extendBasis(basis, p, next) || ++guard > limit)
            return boundingFallback(input);
        basis = next;
        e = encloseBasis(basis);
        haveE = true;
        i = 0;
    }
    return e;
}

bool MapLayout::isFolder(int node) const
{
    return node >= 0 && node < nodes.size() && nodes[node].file < 0;
}

bool MapLayout::contains(int node, const QPointF &point) const
{
    if (node < 0 || node >= nodes.size())
        return false;
    const MapNode &n = nodes[node];
    const double dx = point.x() - n.x;
    const double dy = point.y() - n.y;
    return dx * dx + dy * dy <= n.r * n.r;
}

int MapLayout::nodeAt(const QPointF &point) const
{
    if (nodes.isEmpty() || !contains(0, point))
        return -1;
    int current = 0;
    for (;;) {
        int next = -1;
        for (int child : nodes[current].children) {
            if (contains(child, point)) {
                next = child;
                break;
            }
        }
        if (next < 0)
            return current;
        current = next;
    }
}

QVector<int> MapLayout::chain(int node) const
{
    QVector<int> out;
    while (node >= 0 && node < nodes.size()) {
        out.prepend(node);
        node = nodes[node].parent;
    }
    return out;
}

bool MapLayout::isAncestor(int ancestor, int node) const
{
    while (node >= 0 && node < nodes.size()) {
        if (node == ancestor)
            return true;
        node = nodes[node].parent;
    }
    return false;
}

int MapLayout::fileNode(int file) const
{
    return file >= 0 && file < nodeOfFile.size() ? nodeOfFile[file] : -1;
}

MapLayout buildLayout(const Snapshot &snapshot, const LayoutOptions &options)
{
    MapLayout layout;
    QVector<BuildNode> build;
    build.reserve(snapshot.files.size() * 2 + 1);
    BuildNode root;
    root.name = snapshot.name.isEmpty() ? QStringLiteral("Repository") : snapshot.name;
    build.append(root);
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const QStringList parts = snapshot.files[i].path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;
        int current = 0;
        QString prefix;
        for (int p = 0; p + 1 < parts.size(); ++p) {
            prefix = prefix.isEmpty() ? parts[p] : prefix + QLatin1Char('/') + parts[p];
            const auto found = build[current].folders.constFind(parts[p]);
            if (found != build[current].folders.constEnd()) {
                current = found.value();
                continue;
            }
            BuildNode folder;
            folder.name = parts[p];
            folder.path = prefix;
            build.append(folder);
            const int index = build.size() - 1;
            build[current].folders.insert(parts[p], index);
            build[current].children.append(index);
            current = index;
        }
        BuildNode leaf;
        leaf.name = parts.last();
        leaf.path = snapshot.files[i].path;
        leaf.file = i;
        build.append(leaf);
        build[current].children.append(build.size() - 1);
    }

    layout.nodeOfFile.fill(-1, snapshot.files.size());
    layout.nodes.reserve(build.size());
    struct Pending {
        int build;
        int parent;
        int depth;
    };
    QVector<Pending> stack;
    stack.append({0, -1, 0});
    while (!stack.isEmpty()) {
        const Pending item = stack.takeLast();
        int source = item.build;
        MapNode node;
        node.name = build[source].name;
        node.path = build[source].path;
        node.file = build[source].file;
        node.parent = item.parent;
        node.depth = item.depth;
        if (options.collapseChains && item.parent >= 0 && node.file < 0) {
            while (build[source].children.size() == 1 && build[build[source].children.first()].file < 0) {
                source = build[source].children.first();
                node.name += QLatin1Char('/') + build[source].name;
                node.path = build[source].path;
            }
        }
        layout.nodes.append(node);
        const int index = layout.nodes.size() - 1;
        layout.maxDepth = std::max(layout.maxDepth, node.depth);
        if (item.parent >= 0)
            layout.nodes[item.parent].children.append(index);
        if (node.file >= 0)
            layout.nodeOfFile[node.file] = index;
        const QVector<int> &children = build[source].children;
        for (int c = children.size() - 1; c >= 0; --c)
            stack.append({children[c], index, item.depth + 1});
    }

    for (int i = layout.nodes.size() - 1; i >= 0; --i) {
        MapNode &node = layout.nodes[i];
        if (node.file >= 0) {
            const FileInfo &file = snapshot.files[node.file];
            node.codeLines = file.codeLines;
            node.files = 1;
            node.health = file.health;
            node.commits = file.commits;
            node.r = std::sqrt(std::max<double>(file.codeLines, options.minLines));
            continue;
        }
        double weight = 0;
        qint64 lines = 0;
        for (int child : node.children) {
            const MapNode &c = layout.nodes[child];
            node.codeLines += c.codeLines;
            node.files += c.files;
            node.commits += c.commits;
            const qint64 l = std::max<qint64>(1, c.codeLines);
            lines += l;
            weight += c.health * l;
        }
        node.health = lines > 0 ? weight / lines : 10;
        std::sort(node.children.begin(), node.children.end(), [&layout](int a, int b) {
            const MapNode &na = layout.nodes[a];
            const MapNode &nb = layout.nodes[b];
            if (na.r != nb.r)
                return na.r > nb.r;
            return na.name < nb.name;
        });
        QVector<PackCircle> circles;
        circles.reserve(node.children.size());
        for (int child : node.children)
            circles.append({0, 0, layout.nodes[child].r + options.padding});
        const double enclosing = packSiblings(circles);
        for (int c = 0; c < node.children.size(); ++c) {
            MapNode &child = layout.nodes[node.children[c]];
            child.x = circles[c].x;
            child.y = circles[c].y;
        }
        node.r = enclosing + options.padding + enclosing * options.rimShare;
    }
    for (int i = 1; i < layout.nodes.size(); ++i) {
        MapNode &node = layout.nodes[i];
        const MapNode &parent = layout.nodes[node.parent];
        node.x += parent.x;
        node.y += parent.y;
    }
    return layout;
}

ViewTransform fitTransform(const MapNode &node, const QSizeF &viewport, double margin)
{
    ViewTransform t;
    t.cx = node.x;
    t.cy = node.y;
    const double half = std::max(10.0, std::min(viewport.width(), viewport.height()) / 2 - margin);
    t.k = node.r > 0 ? half / node.r : 1;
    return t;
}

ViewTransform interpolateView(const ViewTransform &from, const ViewTransform &to, double t,
                              const QSizeF &viewport)
{
    const double size = std::max(1.0, std::min(viewport.width(), viewport.height()));
    const double rho = std::sqrt(2.0);
    const double rho2 = 2;
    const double rho4 = 4;
    const double w0 = size / from.k;
    const double w1 = size / to.k;
    const double dx = to.cx - from.cx;
    const double dy = to.cy - from.cy;
    const double d2 = dx * dx + dy * dy;
    ViewTransform out;
    if (d2 < 1e-12) {
        const double s = std::log(w1 / w0) / rho;
        out.cx = from.cx + t * dx;
        out.cy = from.cy + t * dy;
        out.k = size / (w0 * std::exp(rho * t * s));
        return out;
    }
    const double d1 = std::sqrt(d2);
    const double b0 = (w1 * w1 - w0 * w0 + rho4 * d2) / (2 * w0 * rho2 * d1);
    const double b1 = (w1 * w1 - w0 * w0 - rho4 * d2) / (2 * w1 * rho2 * d1);
    const double r0 = std::log(std::sqrt(b0 * b0 + 1) - b0);
    const double r1 = std::log(std::sqrt(b1 * b1 + 1) - b1);
    const double s = t * (r1 - r0) / rho;
    const double coshr0 = std::cosh(r0);
    const double u = w0 / (rho2 * d1) * (coshr0 * std::tanh(rho * s + r0) - std::sinh(r0));
    out.cx = from.cx + u * dx;
    out.cy = from.cy + u * dy;
    out.k = size / (w0 * coshr0 / std::cosh(rho * s + r0));
    if (!std::isfinite(out.cx) || !std::isfinite(out.cy) || !std::isfinite(out.k) || out.k <= 0) {
        out.cx = from.cx + t * dx;
        out.cy = from.cy + t * dy;
        out.k = std::exp(std::log(from.k) + t * (std::log(to.k) - std::log(from.k)));
    }
    if (t >= 1)
        out = to;
    return out;
}

double easeInOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}

int readableFocus(const MapLayout &layout, int node, const QSizeF &viewport, double minRadius)
{
    if (node < 0 || node >= layout.nodes.size())
        return layout.nodes.isEmpty() ? -1 : 0;
    int candidate = layout.isFolder(node) ? node : layout.nodes[node].parent;
    if (candidate < 0)
        return 0;
    int best = candidate;
    while (candidate >= 0) {
        const ViewTransform fit = fitTransform(layout.nodes[candidate], viewport);
        if (layout.nodes[node].r * fit.k < minRadius)
            break;
        best = candidate;
        candidate = layout.nodes[candidate].parent;
    }
    return best;
}

QList<Tab> allTabs()
{
    return {Tab::Hotspots, Tab::RefactoringTargets, Tab::CodeHealth, Tab::ChangeCoupling,
            Tab::Knowledge, Tab::Defects, Tab::Languages};
}

QString tabTitle(Tab tab)
{
    switch (tab) {
    case Tab::Hotspots:
        return QStringLiteral("Hotspots");
    case Tab::RefactoringTargets:
        return QStringLiteral("Refactoring targets");
    case Tab::CodeHealth:
        return QStringLiteral("Code health");
    case Tab::ChangeCoupling:
        return QStringLiteral("Change coupling");
    case Tab::Knowledge:
        return QStringLiteral("Knowledge");
    case Tab::Defects:
        return QStringLiteral("Defects");
    case Tab::Languages:
        return QStringLiteral("Languages");
    }
    return {};
}

bool tabNeedsHistory(Tab tab)
{
    return tab != Tab::CodeHealth && tab != Tab::Languages;
}

bool hasHistory(const Snapshot &snapshot)
{
    if (!snapshot.git)
        return false;
    if (snapshot.commitsScanned > 0)
        return true;
    for (const FileInfo &file : snapshot.files) {
        if (file.commits > 0)
            return true;
    }
    return false;
}

bool Filters::active() const
{
    return minHealth > 1 || maxHealth < 10 || minCommits > 0 || !author.isEmpty()
           || !search.trimmed().isEmpty();
}

bool matchesFilters(const FileInfo &file, const Filters &filters)
{
    if (file.health < filters.minHealth - 1e-9 || file.health > filters.maxHealth + 1e-9)
        return false;
    if (file.commits < filters.minCommits)
        return false;
    if (!filters.author.isEmpty() && file.mainAuthor != filters.author) {
        bool found = false;
        for (const auto &entry : file.topAuthors) {
            if (entry.first == filters.author) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    const QString search = filters.search.trimmed();
    if (!search.isEmpty() && !file.path.contains(search, Qt::CaseInsensitive))
        return false;
    return true;
}

CouplingIndex buildCouplingIndex(const Snapshot &snapshot)
{
    CouplingIndex index(snapshot.files.size());
    for (const insight::Coupling &c : snapshot.couplings) {
        if (c.a < 0 || c.b < 0 || c.a >= index.size() || c.b >= index.size() || c.a == c.b)
            continue;
        index[c.a].append({c.b, c.shared, c.degree});
        index[c.b].append({c.a, c.shared, c.degree});
    }
    for (QVector<CouplingRef> &list : index) {
        std::sort(list.begin(), list.end(), [](const CouplingRef &a, const CouplingRef &b) {
            if (a.degree != b.degree)
                return a.degree > b.degree;
            if (a.shared != b.shared)
                return a.shared > b.shared;
            return a.file < b.file;
        });
    }
    return index;
}

double targetScore(const FileInfo &file)
{
    return std::max(0.0, file.hotspot) * std::max(0.0, 10.0 - file.health);
}

QList<Target> refactoringTargets(const Snapshot &snapshot, const CouplingIndex &coupling, int limit)
{
    QList<Target> targets;
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const FileInfo &file = snapshot.files[i];
        const double s = targetScore(file);
        if (s <= 0.05 || file.commits <= 0)
            continue;
        targets.append({i, 0, s, {}});
    }
    std::sort(targets.begin(), targets.end(), [&snapshot](const Target &a, const Target &b) {
        if (a.score != b.score)
            return a.score > b.score;
        return snapshot.files[a.file].path < snapshot.files[b.file].path;
    });
    if (limit > 0 && targets.size() > limit)
        targets = targets.mid(0, limit);
    const QLocale locale;
    for (int r = 0; r < targets.size(); ++r) {
        Target &t = targets[r];
        t.rank = r + 1;
        const FileInfo &file = snapshot.files[t.file];
        t.reasons << QStringLiteral("Changed in %1 commits (%2 in the last 90 days)")
                         .arg(locale.toString(file.commits))
                         .arg(locale.toString(file.recentCommits));
        t.reasons << QStringLiteral("Code health %1, %2")
                         .arg(locale.toString(file.health, 'f', 1))
                         .arg(insight::healthBandName(insight::healthBand(file.health)).toLower());
        for (int k = 0; k < std::min<int>(2, file.healthIssues.size()); ++k)
            t.reasons << file.healthIssues[k];
        if (file.fixCommits > 0)
            t.reasons << QStringLiteral("%1 bug-fix commits").arg(locale.toString(file.fixCommits));
        if (t.file < coupling.size() && !coupling[t.file].isEmpty())
            t.reasons << QStringLiteral("Changes together with %1 other files")
                             .arg(locale.toString(coupling[t.file].size()));
        if (file.formerAuthorShare >= 0.5)
            t.reasons << QStringLiteral("%1 written by former contributors")
                             .arg(percentText(file.formerAuthorShare));
    }
    return targets;
}

QColor bandColor(HealthBand band)
{
    switch (band) {
    case HealthBand::Healthy:
        return hex("#3fb98f");
    case HealthBand::Problematic:
        return hex("#e3b341");
    case HealthBand::Unhealthy:
        return hex("#e5534b");
    }
    return hex("#3fb98f");
}

QColor healthColor(double health)
{
    return bandColor(insight::healthBand(health));
}

QColor neutralColor()
{
    return hex("#6e7681");
}

QColor couplingLinkColor()
{
    return hex("#f2cc60");
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t),
                            float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t),
                            float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

const QList<QColor> &categoricalPalette()
{
    static const QList<QColor> palette = {hex("#3987e5"), hex("#d95926"), hex("#199e70"),
                                          hex("#c98500"), hex("#d55181"), hex("#008300"),
                                          hex("#9085e9"), hex("#e66767")};
    return palette;
}

QStringList topAuthors(const Snapshot &snapshot, int limit)
{
    QHash<QString, qint64> lines;
    for (const FileInfo &file : snapshot.files) {
        if (!file.mainAuthor.isEmpty())
            lines[file.mainAuthor] += std::max(1, file.codeLines);
    }
    QStringList names = lines.keys();
    std::sort(names.begin(), names.end(), [&lines](const QString &a, const QString &b) {
        const qint64 la = lines.value(a);
        const qint64 lb = lines.value(b);
        return la != lb ? la > lb : a < b;
    });
    return limit > 0 ? names.mid(0, limit) : names;
}

QStringList topLanguages(const Snapshot &snapshot, int limit)
{
    QHash<QString, qint64> lines;
    for (const FileInfo &file : snapshot.files) {
        if (!file.language.isEmpty())
            lines[file.language] += std::max(1, file.codeLines);
    }
    QStringList names = lines.keys();
    std::sort(names.begin(), names.end(), [&lines](const QString &a, const QString &b) {
        const qint64 la = lines.value(a);
        const qint64 lb = lines.value(b);
        return la != lb ? la > lb : a < b;
    });
    return limit > 0 ? names.mid(0, limit) : names;
}

TabColoring colorize(const Snapshot &snapshot, Tab tab, const Filters &filters,
                     const CouplingIndex &coupling)
{
    TabColoring out;
    out.tab = tab;
    const int n = snapshot.files.size();
    out.files.resize(n);
    for (int i = 0; i < n; ++i) {
        out.files[i].color = neutralColor();
        out.files[i].dimmed = !matchesFilters(snapshot.files[i], filters);
    }
    const QColor neutral = neutralColor();
    if (tabNeedsHistory(tab) && !hasHistory(snapshot)) {
        out.note = QStringLiteral("%1 need git history. This folder has none, so only Code health "
                                  "and Languages can be shown.")
                       .arg(tabTitle(tab));
        for (FileStyle &style : out.files)
            style.dimmed = true;
        return out;
    }

    switch (tab) {
    case Tab::Hotspots: {
        int maxCommits = 0;
        for (const FileInfo &file : snapshot.files)
            maxCommits = std::max(maxCommits, file.commits);
        const QColor grey = hex("#6e7681");
        const QColor orange = hex("#f0883e");
        const QColor red = hex("#da3633");
        for (int i = 0; i < n; ++i) {
            const double t = std::pow(logShare(snapshot.files[i].commits, maxCommits), 1.6);
            if (snapshot.files[i].commits <= 0)
                out.files[i].color = neutral;
            else
                out.files[i].color = t < 0.5 ? mix(grey, orange, t / 0.5) : mix(orange, red, (t - 0.5) / 0.5);
        }
        QVector<int> order;
        for (int i = 0; i < n; ++i) {
            if (snapshot.files[i].hotspot > 0)
                order.append(i);
        }
        std::sort(order.begin(), order.end(), [&snapshot](int a, int b) {
            const double ha = snapshot.files[a].hotspot;
            const double hb = snapshot.files[b].hotspot;
            return ha != hb ? ha > hb : snapshot.files[a].path < snapshot.files[b].path;
        });
        for (int r = 0; r < std::min<int>(12, order.size()); ++r) {
            const FileInfo &file = snapshot.files[order[r]];
            if (file.hotspot < 0.35)
                break;
            out.files[order[r]].glow = file.hotspot;
            ++out.glowing;
        }
        out.scale = true;
        out.scaleLow = grey;
        out.scaleHigh = red;
        out.scaleLowLabel = QStringLiteral("Rarely changed");
        out.scaleHighLabel = QStringLiteral("Changed often");
        out.legend << LegendEntry{QStringLiteral("Top hotspot"), hex("#ff7b52"), true};
        break;
    }
    case Tab::RefactoringTargets: {
        out.targets = refactoringTargets(snapshot, coupling, 15);
        QVector<int> rank(n, 0);
        for (const Target &t : out.targets)
            rank[t.file] = t.rank;
        for (int i = 0; i < n; ++i) {
            out.files[i].color = healthColor(snapshot.files[i].health);
            out.files[i].rank = rank[i];
            if (rank[i] == 0)
                out.files[i].dimmed = true;
        }
        out.legend << LegendEntry{QStringLiteral("Unhealthy target"), bandColor(HealthBand::Unhealthy)}
                   << LegendEntry{QStringLiteral("Problematic target"), bandColor(HealthBand::Problematic)};
        if (out.targets.isEmpty())
            out.note = QStringLiteral("No refactoring targets: every frequently changed file is healthy.");
        break;
    }
    case Tab::CodeHealth:
        for (int i = 0; i < n; ++i)
            out.files[i].color = healthColor(snapshot.files[i].health);
        out.legend << LegendEntry{QStringLiteral("Healthy"), bandColor(HealthBand::Healthy)}
                   << LegendEntry{QStringLiteral("Problematic"), bandColor(HealthBand::Problematic)}
                   << LegendEntry{QStringLiteral("Unhealthy"), bandColor(HealthBand::Unhealthy)};
        break;
    case Tab::ChangeCoupling: {
        const QColor weak = hex("#7d6a3a");
        const QColor strong = couplingLinkColor();
        int coupled = 0;
        for (int i = 0; i < n; ++i) {
            if (i >= coupling.size() || coupling[i].isEmpty()) {
                out.files[i].color = neutral;
                continue;
            }
            ++coupled;
            const double d = coupling[i].first().degree;
            out.files[i].color = mix(weak, strong, std::clamp((d - 0.2) / 0.8, 0.0, 1.0));
        }
        out.scale = true;
        out.scaleLow = weak;
        out.scaleHigh = strong;
        out.scaleLowLabel = QStringLiteral("Weak");
        out.scaleHighLabel = QStringLiteral("Strong coupling");
        if (coupled == 0)
            out.note = QStringLiteral("No files change together often enough to count as coupled.");
        break;
    }
    case Tab::Knowledge: {
        const QStringList authors = topAuthors(snapshot, categoricalPalette().size());
        QHash<QString, int> slot;
        for (int a = 0; a < authors.size(); ++a)
            slot.insert(authors[a], a);
        const QSet<QString> former(snapshot.formerAuthors.cbegin(), snapshot.formerAuthors.cend());
        const QColor quiet = hex("#3d444d");
        bool anyQuiet = false;
        for (const FileInfo &file : snapshot.files)
            anyQuiet = anyQuiet || file.mainAuthor.isEmpty();
        for (int i = 0; i < n; ++i) {
            const FileInfo &file = snapshot.files[i];
            const int s = slot.value(file.mainAuthor, -1);
            out.files[i].color = s >= 0 ? categoricalPalette()[s] : file.mainAuthor.isEmpty() ? quiet : neutral;
            out.files[i].marked = file.formerAuthorShare >= 0.5;
        }
        for (int a = 0; a < authors.size(); ++a)
            out.legend << LegendEntry{former.contains(authors[a]) ? authors[a] + QStringLiteral(" (former)")
                                                                  : authors[a],
                                      categoricalPalette()[a]};
        if (topAuthors(snapshot, 0).size() > authors.size())
            out.legend << LegendEntry{QStringLiteral("Other authors"), neutral};
        if (anyQuiet)
            out.legend << LegendEntry{QStringLiteral("No commits in the window"), quiet};
        out.legend << LegendEntry{QStringLiteral("Mostly former contributors"), QColor(), true};
        break;
    }
    case Tab::Defects: {
        int maxFixes = 0;
        for (const FileInfo &file : snapshot.files)
            maxFixes = std::max(maxFixes, file.fixCommits);
        const QColor low = hex("#e3a8a2");
        const QColor high = hex("#d1242f");
        for (int i = 0; i < n; ++i) {
            const int fixes = snapshot.files[i].fixCommits;
            out.files[i].color = fixes > 0 ? mix(low, high, logShare(fixes, maxFixes)) : neutral;
        }
        out.scale = true;
        out.scaleLow = low;
        out.scaleHigh = high;
        out.scaleLowLabel = QStringLiteral("Few fixes");
        out.scaleHighLabel = QStringLiteral("Many fixes");
        if (maxFixes == 0)
            out.note = QStringLiteral("No bug-fix commits found in the history window.");
        break;
    }
    case Tab::Languages: {
        const QStringList languages = topLanguages(snapshot, categoricalPalette().size());
        QHash<QString, int> slot;
        for (int l = 0; l < languages.size(); ++l)
            slot.insert(languages[l], l);
        for (int i = 0; i < n; ++i) {
            const int s = slot.value(snapshot.files[i].language, -1);
            out.files[i].color = s >= 0 ? categoricalPalette()[s] : neutral;
        }
        for (int l = 0; l < languages.size(); ++l)
            out.legend << LegendEntry{languages[l], categoricalPalette()[l]};
        if (topLanguages(snapshot, 0).size() > languages.size())
            out.legend << LegendEntry{QStringLiteral("Other"), neutral};
        break;
    }
    }
    for (const FileStyle &style : out.files)
        out.matching += style.dimmed ? 0 : 1;
    return out;
}

Summary summarize(const Snapshot &snapshot)
{
    Summary s;
    double best = 0;
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const FileInfo &file = snapshot.files[i];
        switch (bandIndex(file.health)) {
        case 0:
            ++s.healthy;
            s.healthyLines += file.codeLines;
            break;
        case 1:
            ++s.problematic;
            s.problematicLines += file.codeLines;
            break;
        default:
            ++s.unhealthy;
            s.unhealthyLines += file.codeLines;
            break;
        }
        if (file.hotspot > best) {
            best = file.hotspot;
            s.topHotspot = i;
        }
    }
    const QLocale locale;
    QStringList parts;
    parts << QStringLiteral("%1 unhealthy").arg(locale.toString(s.unhealthy));
    parts << QStringLiteral("%1 problematic").arg(locale.toString(s.problematic));
    if (s.topHotspot >= 0)
        parts << QStringLiteral("top hotspot %1").arg(snapshot.files[s.topHotspot].path);
    s.text = parts.join(QStringLiteral(" · "));
    return s;
}

QString fileNameOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? path : path.mid(slash + 1);
}

QString folderOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

QString spanText(int days)
{
    if (days >= 330) {
        const int years = std::max(1, int(std::lround(days / 365.0)));
        return years == 1 ? QStringLiteral("1 year") : QStringLiteral("%1 years").arg(years);
    }
    if (days >= 45) {
        const int months = std::max(1, int(std::lround(days / 30.4)));
        return QStringLiteral("%1 months").arg(months);
    }
    return days == 1 ? QStringLiteral("1 day") : QStringLiteral("%1 days").arg(std::max(0, days));
}

QString agoText(qint64 when, qint64 now)
{
    if (when <= 0)
        return QStringLiteral("never");
    const qint64 days = std::max<qint64>(0, (now - when) / 86400);
    if (days == 0)
        return QStringLiteral("today");
    if (days == 1)
        return QStringLiteral("yesterday");
    if (days < 14)
        return QStringLiteral("%1 days ago").arg(days);
    if (days < 60)
        return QStringLiteral("%1 weeks ago").arg(days / 7);
    if (days < 365)
        return QStringLiteral("%1 months ago").arg(std::max<qint64>(2, std::lround(days / 30.4)));
    const qint64 years = std::lround(days / 365.0);
    return years <= 1 ? QStringLiteral("1 year ago") : QStringLiteral("%1 years ago").arg(years);
}

QString percentText(double share)
{
    return QStringLiteral("%1 %").arg(int(std::lround(std::clamp(share, 0.0, 1.0) * 100)));
}

} // namespace forkmesh::ember
