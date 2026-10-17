#include "SynapseLayout.h"

#include <QElapsedTimer>
#include <QSet>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace forkmesh::synapse {

namespace {

constexpr float kGoldenAngle = 2.39996323f;

float unitFromHash(quint32 value)
{
    return float(value & 0xffffffu) / float(0xffffff);
}

quint32 mixHash(quint32 value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

QVector3D jitter(quint32 seed)
{
    const float u = unitFromHash(mixHash(seed ^ 0x9e3779b9u));
    const float v = unitFromHash(mixHash(seed ^ 0x85ebca6bu));
    const float w = unitFromHash(mixHash(seed ^ 0xc2b2ae35u));
    const float theta = u * 6.2831853f;
    const float z = v * 2.0f - 1.0f;
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const float radius = std::cbrt(std::max(0.02f, w));
    return QVector3D(r * std::cos(theta), z, r * std::sin(theta)) * radius;
}

QString directoryKey(const QString &path, int depth)
{
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() <= 1)
        return QString();
    const int take = std::min(depth, int(parts.size()) - 1);
    return parts.mid(0, take).join(QLatin1Char('/'));
}

struct Grouping {
    QVector<int> groupOfFile;
    QStringList paths;
    QStringList names;
    QVector<int> layers;
};

Grouping fallbackGrouping(const insight::Snapshot &snapshot, const QVector<int> &files)
{
    const int wanted = std::clamp(int(files.size()) / 40, 3, 14);
    int chosen = 1;
    for (int depth = 1; depth <= 4; ++depth) {
        QSet<QString> keys;
        for (int file : files)
            keys.insert(directoryKey(snapshot.files.at(file).path, depth));
        chosen = depth;
        if (keys.size() >= wanted)
            break;
    }
    Grouping grouping;
    grouping.groupOfFile.fill(-1, snapshot.files.size());
    QHash<QString, int> index;
    QVector<int> sorted = files;
    std::sort(sorted.begin(), sorted.end(), [&](int a, int b) {
        return snapshot.files.at(a).path < snapshot.files.at(b).path;
    });
    for (int file : sorted) {
        const QString key = directoryKey(snapshot.files.at(file).path, chosen);
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.insert(key, grouping.paths.size());
            grouping.paths.append(key);
            grouping.names.append(key.isEmpty() ? QStringLiteral("(root)") : key);
            grouping.layers.append(0);
        }
        grouping.groupOfFile[file] = it.value();
    }
    return grouping;
}

Grouping moduleGrouping(const insight::Snapshot &snapshot, const QVector<int> &files)
{
    const insight::ModuleGraph modules = insight::buildModuleGraph(snapshot, 0);
    if (modules.moduleOfFile.size() != snapshot.files.size() || modules.nodes.isEmpty())
        return fallbackGrouping(snapshot, files);
    Grouping grouping;
    grouping.groupOfFile.fill(-1, snapshot.files.size());
    QHash<int, int> remap;
    for (int file : files) {
        const int module = modules.moduleOfFile.at(file);
        if (module < 0 || module >= modules.nodes.size())
            return fallbackGrouping(snapshot, files);
        auto it = remap.find(module);
        if (it == remap.end()) {
            it = remap.insert(module, grouping.paths.size());
            const insight::ModuleNode &node = modules.nodes.at(module);
            grouping.paths.append(node.path);
            QString name = node.path.isEmpty() ? node.name : node.path;
            grouping.names.append(name.isEmpty() ? QStringLiteral("(root)") : name);
            grouping.layers.append(node.layer);
        }
        grouping.groupOfFile[file] = it.value();
    }
    return grouping;
}

QVector3D shellNormal(const BrainShape &shape, const QVector3D &p)
{
    const float b = p.y() >= 0 ? shape.halfHeightTop : shape.halfHeightBottom;
    QVector3D n(p.x() / (shape.halfWidth * shape.halfWidth), p.y() / (b * b),
                p.z() / (shape.halfLength * shape.halfLength));
    const float len = n.length();
    return len > 1e-6f ? n / len : QVector3D(0, 1, 0);
}

void placeLobes(Graph &graph)
{
    const int count = graph.lobes.size();
    if (count == 0)
        return;
    const BrainShape &shape = graph.shape;
    if (count == 1) {
        graph.lobes[0].anchor = QVector3D(0, 0, 0);
        graph.lobes[0].hemisphere = 0;
        return;
    }
    QVector<int> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const Lobe &la = graph.lobes.at(a);
        const Lobe &lb = graph.lobes.at(b);
        if (la.layer != lb.layer)
            return la.layer < lb.layer;
        if (la.nodes.size() != lb.nodes.size())
            return la.nodes.size() > lb.nodes.size();
        return la.path < lb.path;
    });
    QVector<QVector3D> points(count);
    for (int i = 0; i < count; ++i) {
        const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(count);
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float phi = float(i) * kGoldenAngle;
        points[i] = QVector3D(r * std::sin(phi), y * 0.8f, r * std::cos(phi));
    }
    std::sort(points.begin(), points.end(), [](const QVector3D &a, const QVector3D &b) {
        if (a.z() != b.z())
            return a.z() < b.z();
        return a.x() < b.x();
    });
    QVector<QVector3D> anchors(count);
    for (int i = 0; i < count; ++i)
        anchors[order.at(i)] = points.at(i);

    QHash<quint64, int> weights;
    for (const Edge &edge : graph.edges) {
        const int a = graph.nodes.at(edge.from).lobe;
        const int b = graph.nodes.at(edge.to).lobe;
        if (a == b)
            continue;
        const quint64 key = (quint64(std::min(a, b)) << 32) | quint64(std::max(a, b));
        weights[key] += 1;
    }
    QVector<std::pair<std::pair<int, int>, float>> springs;
    for (auto it = weights.constBegin(); it != weights.constEnd(); ++it)
        springs.append({{int(it.key() >> 32), int(it.key() & 0xffffffffu)},
                        std::log1p(float(it.value()))});
    std::sort(springs.begin(), springs.end());
    QVector<float> mass(count);
    float totalMass = 0;
    for (int i = 0; i < count; ++i) {
        mass[i] = std::sqrt(float(graph.lobes.at(i).nodes.size()) + 1.0f);
        totalMass += mass[i];
    }
    const float ideal = 2.0f * std::sqrt(4.0f * 3.14159f / float(count));
    for (int iteration = 0; iteration < 160; ++iteration) {
        const float temp = 0.12f * (1.0f - float(iteration) / 160.0f) + 0.004f;
        QVector<QVector3D> force(count);
        for (int i = 0; i < count; ++i) {
            for (int j = i + 1; j < count; ++j) {
                QVector3D d = anchors.at(i) - anchors.at(j);
                const float dist2 = std::max(d.lengthSquared(), 1e-4f);
                const float strength = ideal * ideal * mass.at(i) * mass.at(j)
                                       / (totalMass / float(count)) / (totalMass / float(count));
                const QVector3D f = d * (strength / dist2) * 0.05f;
                force[i] += f;
                force[j] -= f;
            }
        }
        for (const auto &spring : springs) {
            const int a = spring.first.first;
            const int b = spring.first.second;
            const QVector3D d = anchors.at(b) - anchors.at(a);
            const QVector3D f = d * (0.04f * spring.second);
            force[a] += f;
            force[b] -= f;
        }
        for (int i = 0; i < count; ++i) {
            QVector3D f = force.at(i);
            const float len = f.length();
            if (len > temp)
                f *= temp / len;
            QVector3D p = anchors.at(i) + f;
            const float plen = p.length();
            anchors[i] = plen > 1e-5f ? p / plen : QVector3D(0, 1, 0);
        }
    }
    const float middle = 0.5f * (shape.innerShell + shape.outerShell);
    for (int i = 0; i < count; ++i) {
        QVector3D direction = anchors.at(i);
        if (std::abs(direction.x()) < 0.25f) {
            const float sign = direction.x() < 0 ? -1.0f : direction.x() > 0 ? 1.0f
                                                       : (i % 2 == 0 ? 1.0f : -1.0f);
            direction.setX(0.25f * sign);
            direction.normalize();
        }
        if (direction.y() < -0.6f) {
            direction.setY(-0.6f);
            direction.normalize();
        }
        Lobe &lobe = graph.lobes[i];
        lobe.anchor = shape.onShell(direction, middle);
        lobe.hemisphere = lobe.anchor.x() < 0 ? -1 : 1;
    }
}

struct Cell {
    QVector3D centre;
    QVector3D boxCentre;
    float mass = 0;
    float size = 0;
    int children[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int begin = 0;
    int end = 0;
    bool leaf = true;
};

class Octree
{
public:
    Octree(const QVector<QVector3D> &points)
        : m_points(points)
    {
        const int n = points.size();
        m_order.resize(n);
        std::iota(m_order.begin(), m_order.end(), 0);
        m_scratch.resize(n);
        if (n == 0)
            return;
        QVector3D lo = points.at(0);
        QVector3D hi = points.at(0);
        for (const QVector3D &p : points) {
            lo = QVector3D(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
            hi = QVector3D(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
        }
        const QVector3D span = hi - lo;
        const float size = std::max({span.x(), span.y(), span.z(), 1e-3f}) * 1.001f;
        m_cells.reserve(n * 2);
        build(0, n, (lo + hi) * 0.5f, size, 0);
    }

    QVector3D force(int body, float k2, float theta2) const
    {
        QVector3D total;
        if (m_cells.isEmpty())
            return total;
        const QVector3D p = m_points.at(body);
        int stack[512];
        int top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const Cell &cell = m_cells.at(stack[--top]);
            if (cell.leaf) {
                for (int i = cell.begin; i < cell.end; ++i) {
                    const int other = m_order.at(i);
                    if (other == body)
                        continue;
                    const QVector3D d = p - m_points.at(other);
                    const float dist2 = std::max(d.lengthSquared(), 0.01f * k2);
                    total += d * (k2 / dist2);
                }
                continue;
            }
            const QVector3D d = p - cell.centre;
            const float dist2 = d.lengthSquared();
            if (cell.size * cell.size < theta2 * dist2) {
                total += d * (cell.mass * k2 / std::max(dist2, 0.01f * k2));
                continue;
            }
            for (int child : cell.children) {
                if (child >= 0 && top < 511)
                    stack[top++] = child;
            }
        }
        return total;
    }

private:
    int build(int begin, int end, const QVector3D &boxCentre, float size, int depth)
    {
        const int index = m_cells.size();
        m_cells.append(Cell());
        Cell cell;
        cell.boxCentre = boxCentre;
        cell.size = size;
        cell.begin = begin;
        cell.end = end;
        QVector3D sum;
        for (int i = begin; i < end; ++i)
            sum += m_points.at(m_order.at(i));
        cell.mass = float(end - begin);
        cell.centre = sum / std::max(1.0f, cell.mass);
        if (end - begin <= 4 || depth >= 24) {
            cell.leaf = true;
            m_cells[index] = cell;
            return index;
        }
        cell.leaf = false;
        int counts[8] = {0};
        for (int i = begin; i < end; ++i)
            ++counts[octant(m_points.at(m_order.at(i)), boxCentre)];
        int starts[9];
        starts[0] = begin;
        for (int o = 0; o < 8; ++o)
            starts[o + 1] = starts[o] + counts[o];
        int cursor[8];
        std::copy(starts, starts + 8, cursor);
        for (int i = begin; i < end; ++i) {
            const int body = m_order.at(i);
            m_scratch[cursor[octant(m_points.at(body), boxCentre)]++] = body;
        }
        for (int i = begin; i < end; ++i)
            m_order[i] = m_scratch.at(i);
        const float half = size * 0.5f;
        const float quarter = size * 0.25f;
        for (int o = 0; o < 8; ++o) {
            if (counts[o] == 0)
                continue;
            const QVector3D childCentre = boxCentre
                + QVector3D((o & 1) ? quarter : -quarter, (o & 2) ? quarter : -quarter,
                            (o & 4) ? quarter : -quarter);
            cell.children[o] = build(starts[o], starts[o + 1], childCentre, half, depth + 1);
        }
        m_cells[index] = cell;
        return index;
    }

    static int octant(const QVector3D &p, const QVector3D &c)
    {
        return (p.x() >= c.x() ? 1 : 0) | (p.y() >= c.y() ? 2 : 0) | (p.z() >= c.z() ? 4 : 0);
    }

    const QVector<QVector3D> &m_points;
    QVector<int> m_order;
    QVector<int> m_scratch;
    QVector<Cell> m_cells;
};

}

float BrainShape::radiusOf(const QVector3D &point) const
{
    const float length = point.length();
    if (length < 1e-6f)
        return 0;
    const float taper = 1.0f - 0.1f * (point.z() / length);
    const float a = halfWidth * taper;
    const float b = point.y() >= 0 ? halfHeightTop : halfHeightBottom;
    const float x = point.x() / a;
    const float y = point.y() / b;
    const float z = point.z() / halfLength;
    return std::sqrt(x * x + y * y + z * z);
}

float BrainShape::outerAt(const QVector3D &point) const
{
    const float length = point.length();
    if (length < 1e-6f)
        return outerShell;
    const QVector3D d = point / length;
    const float theta = std::atan2(d.y(), d.z());
    const float phi = std::asin(std::clamp(d.x(), -1.0f, 1.0f));
    const float groove = std::sin(10.0f * theta + 2.6f * std::sin(5.0f * phi + 1.3f * theta));
    return outerShell - 0.055f * (0.5f + 0.5f * groove);
}

QVector3D BrainShape::onShell(const QVector3D &direction, float radius) const
{
    const float r = radiusOf(direction);
    if (r < 1e-6f)
        return QVector3D();
    return direction * (radius / r);
}

quint32 stableHash(const QString &text)
{
    quint32 hash = 2166136261u;
    for (const QChar ch : text) {
        hash ^= ch.unicode();
        hash *= 16777619u;
    }
    return mixHash(hash);
}

QString fileName(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? path : path.mid(slash + 1);
}

int importCount(const insight::Snapshot &snapshot)
{
    int total = 0;
    for (int i = 0; i < snapshot.files.size(); ++i) {
        for (int target : snapshot.files.at(i).dependsOn) {
            if (target != i)
                ++total;
        }
    }
    return total;
}

QVector<int> cyclePath(const insight::Snapshot &snapshot, const QVector<int> &members)
{
    if (members.isEmpty())
        return {};
    QSet<int> inside(members.begin(), members.end());
    int start = members.first();
    for (int member : members) {
        if (member >= 0 && member < snapshot.files.size()
            && snapshot.files.at(member).path < snapshot.files.at(start).path)
            start = member;
    }
    if (start < 0 || start >= snapshot.files.size())
        return {};
    QHash<int, int> parent;
    QVector<int> queue{start};
    parent.insert(start, -1);
    int closing = -1;
    for (int head = 0; head < queue.size() && closing < 0; ++head) {
        const int current = queue.at(head);
        QVector<int> next = snapshot.files.at(current).dependsOn;
        std::sort(next.begin(), next.end(), [&](int a, int b) {
            return snapshot.files.at(a).path < snapshot.files.at(b).path;
        });
        for (int target : next) {
            if (!inside.contains(target))
                continue;
            if (target == start) {
                closing = current;
                break;
            }
            if (parent.contains(target))
                continue;
            parent.insert(target, current);
            queue.append(target);
        }
    }
    if (closing < 0)
        return {};
    QVector<int> path;
    for (int at = closing; at >= 0; at = parent.value(at, -1))
        path.prepend(at);
    path.append(start);
    return path;
}

QString cycleText(const insight::Snapshot &snapshot, const QVector<int> &path)
{
    QStringList names;
    for (int file : path) {
        if (file >= 0 && file < snapshot.files.size())
            names.append(fileName(snapshot.files.at(file).path));
    }
    return names.join(QStringLiteral(" → "));
}

QList<CycleInfo> cycleInfos(const insight::Snapshot &snapshot)
{
    QList<CycleInfo> infos;
    for (int c = 0; c < snapshot.cycles.size(); ++c) {
        CycleInfo info;
        info.index = c;
        info.files = snapshot.cycles.at(c);
        info.path = cyclePath(snapshot, info.files);
        infos.append(info);
    }
    return infos;
}

QVector<int> orphans(const insight::Snapshot &snapshot)
{
    QVector<int> result;
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const insight::FileInfo &file = snapshot.files.at(i);
        const bool imports = std::any_of(file.dependsOn.begin(), file.dependsOn.end(),
                                         [i](int t) { return t != i; });
        const bool imported = std::any_of(file.dependedOnBy.begin(), file.dependedOnBy.end(),
                                          [i](int t) { return t != i; });
        if (!imports && !imported)
            result.append(i);
    }
    std::sort(result.begin(), result.end(), [&](int a, int b) {
        return snapshot.files.at(a).path < snapshot.files.at(b).path;
    });
    return result;
}

QVector<int> leaves(const insight::Snapshot &snapshot)
{
    QVector<int> result;
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const insight::FileInfo &file = snapshot.files.at(i);
        const bool imports = std::any_of(file.dependsOn.begin(), file.dependsOn.end(),
                                         [i](int t) { return t != i; });
        const bool imported = std::any_of(file.dependedOnBy.begin(), file.dependedOnBy.end(),
                                          [i](int t) { return t != i; });
        if (!imports && imported)
            result.append(i);
    }
    return result;
}

namespace {

QVector<int> ranked(const insight::Snapshot &snapshot, int limit, bool incoming)
{
    QVector<int> result;
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const insight::FileInfo &file = snapshot.files.at(i);
        if (!(incoming ? file.dependedOnBy : file.dependsOn).isEmpty())
            result.append(i);
    }
    std::sort(result.begin(), result.end(), [&](int a, int b) {
        const insight::FileInfo &fa = snapshot.files.at(a);
        const insight::FileInfo &fb = snapshot.files.at(b);
        const int ca = (incoming ? fa.dependedOnBy : fa.dependsOn).size();
        const int cb = (incoming ? fb.dependedOnBy : fb.dependsOn).size();
        if (ca != cb)
            return ca > cb;
        return fa.path < fb.path;
    });
    if (limit >= 0 && result.size() > limit)
        result.resize(limit);
    return result;
}

}

QVector<int> mostImported(const insight::Snapshot &snapshot, int limit)
{
    return ranked(snapshot, limit, true);
}

QVector<int> mostImporting(const insight::Snapshot &snapshot, int limit)
{
    return ranked(snapshot, limit, false);
}

int mostConnected(const insight::Snapshot &snapshot)
{
    int best = -1;
    double bestScore = 0;
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const insight::FileInfo &file = snapshot.files.at(i);
        const double in = double(file.dependedOnBy.size());
        const double out = double(file.dependsOn.size());
        const double score = std::log1p(in) * std::log1p(out) + 0.05 * std::log1p(in + out)
                             + (file.cycle >= 0 ? 0.5 : 0.0);
        if (score > bestScore
            || (score == bestScore && best >= 0 && score > 0
                && file.path < snapshot.files.at(best).path)) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

Neighbourhood neighbourhood(const insight::Snapshot &snapshot, int file, int hops,
                            Direction direction)
{
    Neighbourhood result;
    if (file < 0 || file >= snapshot.files.size() || hops <= 0)
        return result;
    const auto walk = [&](bool forward, QHash<int, int> &reached) {
        QVector<int> frontier{file};
        for (int depth = 1; depth <= hops && !frontier.isEmpty(); ++depth) {
            QVector<int> next;
            for (int current : frontier) {
                const insight::FileInfo &info = snapshot.files.at(current);
                for (int target : forward ? info.dependsOn : info.dependedOnBy) {
                    if (target == file || reached.contains(target) || target < 0
                        || target >= snapshot.files.size())
                        continue;
                    reached.insert(target, depth);
                    next.append(target);
                }
            }
            frontier = next;
        }
    };
    if (direction != Direction::Importers)
        walk(true, result.imports);
    if (direction != Direction::Imports)
        walk(false, result.importers);
    return result;
}

Graph buildGraph(const insight::SnapshotPtr &snapshot, bool includeIsolated)
{
    Graph graph;
    graph.snapshot = snapshot;
    graph.includesIsolated = includeIsolated;
    if (!snapshot)
        return graph;
    const insight::Snapshot &snap = *snapshot;
    graph.nodeOfFile.fill(-1, snap.files.size());
    QVector<int> files;
    for (int i = 0; i < snap.files.size(); ++i) {
        const insight::FileInfo &file = snap.files.at(i);
        const bool linked = std::any_of(file.dependsOn.begin(), file.dependsOn.end(),
                                        [i](int t) { return t != i; })
                            || std::any_of(file.dependedOnBy.begin(), file.dependedOnBy.end(),
                                           [i](int t) { return t != i; });
        if (linked || includeIsolated)
            files.append(i);
    }
    const Grouping grouping = moduleGrouping(snap, files);
    for (int g = 0; g < grouping.paths.size(); ++g) {
        Lobe lobe;
        lobe.path = grouping.paths.at(g);
        lobe.name = grouping.names.at(g);
        lobe.layer = grouping.layers.at(g);
        graph.lobes.append(lobe);
    }
    std::sort(files.begin(), files.end(), [&](int a, int b) {
        return snap.files.at(a).path < snap.files.at(b).path;
    });
    for (int file : files) {
        const insight::FileInfo &info = snap.files.at(file);
        Node node;
        node.file = file;
        node.lobe = std::max(0, grouping.groupOfFile.at(file));
        node.seed = stableHash(info.path);
        node.cycle = info.cycle >= 0;
        graph.nodeOfFile[file] = graph.nodes.size();
        graph.lobes[node.lobe].nodes.append(graph.nodes.size());
        graph.nodes.append(node);
    }
    graph.outEdges.resize(graph.nodes.size());
    graph.inEdges.resize(graph.nodes.size());
    for (int n = 0; n < graph.nodes.size(); ++n) {
        const int file = graph.nodes.at(n).file;
        const insight::FileInfo &info = snap.files.at(file);
        QVector<int> targets = info.dependsOn;
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (int target : targets) {
            const int to = graph.nodeFor(target);
            if (to < 0 || to == n)
                continue;
            Edge edge;
            edge.from = n;
            edge.to = to;
            edge.cycle = info.cycle >= 0 && info.cycle == snap.files.at(target).cycle;
            edge.seed = mixHash(graph.nodes.at(n).seed ^ (graph.nodes.at(to).seed * 31u));
            graph.outEdges[n].append(graph.edges.size());
            graph.inEdges[to].append(graph.edges.size());
            graph.edges.append(edge);
        }
    }
    graph.imports = graph.edges.size();
    for (Node &node : graph.nodes) {
        const int n = int(&node - graph.nodes.data());
        node.fanIn = graph.inEdges.at(n).size();
        node.fanOut = graph.outEdges.at(n).size();
        const insight::FileInfo &info = snap.files.at(node.file);
        const float loc = float(std::max(info.codeLines, info.lines / 2));
        node.size = std::clamp(0.55f + 0.1f * std::log1p(loc) + 0.2f * std::sqrt(float(node.fanIn)),
                               0.5f, 3.2f);
    }
    graph.cycles = cycleInfos(snap);
    QHash<int, int> cycleOfInfo;
    for (int c = 0; c < graph.cycles.size(); ++c)
        cycleOfInfo.insert(graph.cycles.at(c).index, c);
    for (int e = 0; e < graph.edges.size(); ++e) {
        const Edge &edge = graph.edges.at(e);
        if (!edge.cycle)
            continue;
        const int c = cycleOfInfo.value(snap.files.at(graph.nodes.at(edge.from).file).cycle, -1);
        if (c >= 0)
            graph.cycles[c].edges.append(e);
    }
    const BrainShape &shape = graph.shape;
    const float volume = 4.18879f * shape.halfWidth * shape.halfLength
                         * 0.5f * (shape.halfHeightTop + shape.halfHeightBottom)
                         * (1.0f - std::pow(shape.innerShell, 3.0f));
    graph.spacing = std::clamp(std::cbrt(volume / float(std::max(1, int(graph.nodes.size())))),
                               2.0f, 40.0f) * 0.9f;
    placeLobes(graph);
    for (Node &node : graph.nodes) {
        const Lobe &lobe = graph.lobes.at(node.lobe);
        node.hemisphere = lobe.hemisphere != 0 ? lobe.hemisphere : ((node.seed >> 7) & 1 ? 1 : -1);
    }
    return graph;
}

QVector<QVector3D> seedPositions(const Graph &graph)
{
    QVector<QVector3D> positions(graph.nodes.size());
    const BrainShape &shape = graph.shape;
    for (int n = 0; n < graph.nodes.size(); ++n) {
        const Node &node = graph.nodes.at(n);
        const Lobe &lobe = graph.lobes.at(node.lobe);
        const float spread = graph.spacing * std::cbrt(float(lobe.nodes.size()) + 1.0f) * 0.7f;
        QVector3D p;
        if (graph.lobes.size() == 1) {
            QVector3D direction = jitter(node.seed);
            if (direction.lengthSquared() < 1e-4f)
                direction = QVector3D(0, 1, 0);
            direction.setX(std::abs(direction.x()) * float(node.hemisphere) + 0.15f * node.hemisphere);
            p = shape.onShell(direction.normalized(), 0.55f + 0.35f * unitFromHash(mixHash(node.seed)));
        } else {
            p = lobe.anchor + jitter(node.seed) * spread;
        }
        if (p.x() * float(node.hemisphere) < shape.fissure)
            p.setX(float(node.hemisphere) * (shape.fissure + unitFromHash(node.seed) * 2.0f));
        const float r = shape.radiusOf(p);
        const float outer = shape.outerAt(p);
        if (r > outer)
            p *= outer / r;
        positions[n] = p;
    }
    return positions;
}

int layoutIterations(const Graph &graph, const LayoutOptions &options)
{
    const int n = graph.nodes.size();
    if (n <= 5000)
        return options.iterations;
    return std::max(options.iterations * 2 / 5, options.iterations * 5000 / n);
}

QVector<QVector3D> lobeCentres(const Graph &graph, const QVector<QVector3D> &positions)
{
    QVector<QVector3D> centres(graph.lobes.size());
    for (int l = 0; l < graph.lobes.size(); ++l) {
        const Lobe &lobe = graph.lobes.at(l);
        if (lobe.nodes.isEmpty() || positions.size() != graph.nodes.size()) {
            centres[l] = lobe.anchor;
            continue;
        }
        QVector3D sum;
        for (int n : lobe.nodes)
            sum += positions.at(n);
        centres[l] = sum / float(lobe.nodes.size());
    }
    return centres;
}

QVector<QVector3D> runLayout(const Graph &graph, const LayoutOptions &options,
                             const PublishFn &publish, const StopFn &stop)
{
    QVector<QVector3D> positions = seedPositions(graph);
    const int n = positions.size();
    if (n == 0) {
        if (publish)
            publish(positions, 1.0);
        return positions;
    }
    const BrainShape &shape = graph.shape;
    const float k = graph.spacing;
    const float k2 = k * k;
    const float theta2 = options.theta * options.theta;
    const int iterations = std::max(1, layoutIterations(graph, options));
    const bool barnesHut = n > options.barnesHutAbove;
    const bool lobePull = graph.lobes.size() > 1;
    const float startTemp = 0.06f * shape.halfLength + 1.5f * k;
    const float endTemp = 0.02f * k;
    const int chunkSize = 256;
    QVector<std::pair<int, int>> chunks;
    for (int begin = 0; begin < n; begin += chunkSize)
        chunks.append({begin, std::min(n, begin + chunkSize)});
    QVector<QVector3D> force(n);
    QElapsedTimer clock;
    clock.start();
    if (publish)
        publish(positions, 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        if (stop && stop())
            return positions;
        const float progress = float(iteration) / float(iterations);
        const float temp = startTemp * std::pow(1.0f - progress, 2.0f) + endTemp;
        std::unique_ptr<Octree> tree;
        if (barnesHut)
            tree = std::make_unique<Octree>(positions);
        const auto repulse = [&](const std::pair<int, int> &chunk) {
            for (int i = chunk.first; i < chunk.second; ++i) {
                if (tree) {
                    force[i] = tree->force(i, k2, theta2);
                    continue;
                }
                QVector3D total;
                const QVector3D p = positions.at(i);
                for (int j = 0; j < n; ++j) {
                    if (j == i)
                        continue;
                    const QVector3D d = p - positions.at(j);
                    const float dist2 = std::max(d.lengthSquared(), 0.01f * k2);
                    total += d * (k2 / dist2);
                }
                force[i] = total;
            }
        };
        if (n > 300)
            QtConcurrent::blockingMap(chunks, repulse);
        else
            for (const auto &chunk : chunks)
                repulse(chunk);
        for (const Edge &edge : graph.edges) {
            const QVector3D d = positions.at(edge.to) - positions.at(edge.from);
            const float dist = d.length();
            if (dist < 1e-4f)
                continue;
            const float magnitude = 1.1f * k * std::log(std::max(dist, 0.3f * k) / k);
            const QVector3D f = d * (magnitude / dist);
            force[edge.from] += f;
            force[edge.to] -= f;
        }
        for (int i = 0; i < n; ++i) {
            const Node &node = graph.nodes.at(i);
            QVector3D p = positions.at(i);
            QVector3D f = force.at(i);
            if (lobePull)
                f += (graph.lobes.at(node.lobe).anchor - p) * 0.05f;
            const float r = shape.radiusOf(p);
            const QVector3D normal = shellNormal(shape, p);
            const float outer = shape.outerAt(p);
            if (r > outer)
                f -= normal * ((r - outer) * shape.halfLength * 2.5f);
            else if (r < shape.innerShell)
                f += normal * ((shape.innerShell - r) * shape.halfLength * 2.0f);
            const float side = p.x() * float(node.hemisphere);
            if (side < shape.fissure)
                f.setX(f.x() + float(node.hemisphere) * (shape.fissure - side) * 2.0f);
            const float len = f.length();
            if (len > 1e-6f)
                p += f * (std::min(len, temp) / len);
            const float after = shape.radiusOf(p);
            if (after > 0.99f)
                p *= 0.99f / after;
            if (p.x() * float(node.hemisphere) < 0.5f * shape.fissure)
                p.setX(float(node.hemisphere) * 0.5f * shape.fissure);
            positions[i] = p;
        }
        if (publish && clock.elapsed() >= options.publishIntervalMs) {
            publish(positions, double(iteration + 1) / double(iterations));
            clock.restart();
        }
    }
    if (publish)
        publish(positions, 1.0);
    return positions;
}

}
