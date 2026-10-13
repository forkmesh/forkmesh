#include "ProcessMemory.h"

#include "SystemPressure.h"
#include "SystemStats.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>

#ifdef Q_OS_LINUX
#include <malloc.h>
#endif

namespace forkmesh {
namespace memory {

namespace {

// An anonymous mapping this large is worth a row of its own: one of these is a
// pixmap, a model's backing store or a decoded image, and watching it appear
// is the whole point of the page. Smaller ones fold into a single row so the
// tree stays readable when the allocator holds a few thousand of them.
constexpr qint64 kNamedAnonymousBytes = 8 * 1024 * 1024;

QByteArray readProcFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    // /proc reports size 0; readAll() still drains it.
    return f.readAll();
}

bool isMappingHeader(const QByteArray &line, quint64 *start, QString *permissions,
                     QString *path)
{
    // "7f2c1a000000-7f2c1a021000 rw-p 00000000 00:00 0    /usr/lib/libc.so.6"
    const int dash = line.indexOf('-');
    if (dash <= 0)
        return false;
    const int firstSpace = line.indexOf(' ');
    if (firstSpace < dash)
        return false;
    bool ok = false;
    const quint64 begin = line.left(dash).toULongLong(&ok, 16);
    if (!ok)
        return false;
    const QByteArray perms = line.mid(firstSpace + 1, 4);
    if (perms.size() != 4)
        return false;
    for (char c : perms) {
        if (c != 'r' && c != 'w' && c != 'x' && c != 'p' && c != 's' && c != '-')
            return false;
    }
    if (start)
        *start = begin;
    if (permissions)
        *permissions = QString::fromLatin1(perms);
    // Five whitespace-separated fields, then the rest of the line is the path,
    // which may itself contain spaces.
    int field = 0;
    int at = 0;
    while (at < line.size() && field < 5) {
        while (at < line.size() && line.at(at) != ' ')
            ++at;
        while (at < line.size() && line.at(at) == ' ')
            ++at;
        ++field;
    }
    if (path)
        *path = QString::fromUtf8(line.mid(at)).trimmed();
    return true;
}

// "Rss:  1234 kB" -> 1234 * 1024. Returns false for a line that is not this
// key, so the caller can try the next one.
bool kilobyteLine(const QByteArray &line, const char *key, qint64 *out)
{
    const int keyLength = int(qstrlen(key));
    if (line.size() <= keyLength || !line.startsWith(key) ||
        line.at(keyLength) != ':')
        return false;
    bool ok = false;
    const qint64 kb = line.mid(keyLength + 1).trimmed().split(' ').first()
                          .toLongLong(&ok);
    if (!ok)
        return false;
    *out = kb * 1024;
    return true;
}

QString stripDeleted(const QString &path)
{
    if (path.endsWith(QStringLiteral(" (deleted)")))
        return path.left(path.size() - 10);
    return path;
}

bool hasAnySuffix(const QString &path, std::initializer_list<const char *> suffixes)
{
    for (const char *suffix : suffixes) {
        if (path.endsWith(QLatin1String(suffix), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace

QString groupTitle(MappingGroup group)
{
    switch (group) {
    case MappingGroup::Heap: return QStringLiteral("Heap");
    case MappingGroup::Anonymous: return QStringLiteral("Anonymous memory");
    case MappingGroup::ThreadStacks: return QStringLiteral("Thread stacks");
    case MappingGroup::MainStack: return QStringLiteral("Main thread stack");
    case MappingGroup::SharedMemory: return QStringLiteral("Shared memory");
    case MappingGroup::Graphics: return QStringLiteral("Graphics driver");
    case MappingGroup::Fonts: return QStringLiteral("Fonts");
    case MappingGroup::Libraries: return QStringLiteral("Shared libraries");
    case MappingGroup::Executable: return QStringLiteral("Application binary");
    case MappingGroup::MappedFiles: return QStringLiteral("Mapped files");
    case MappingGroup::Reserved: return QStringLiteral("Reserved address space");
    case MappingGroup::Kernel: return QStringLiteral("Kernel mappings");
    case MappingGroup::GroupCount: break;
    }
    return QStringLiteral("Other");
}

QString groupDescription(MappingGroup group)
{
    switch (group) {
    case MappingGroup::Heap:
        return QStringLiteral(
            "What malloc grew with brk(). Grows when the app allocates and "
            "usually does not shrink when it frees: compare with the "
            "allocator's in-use figure below before calling it a leak.");
    case MappingGroup::Anonymous:
        return QStringLiteral(
            "Private writable mappings with no file behind them: large "
            "mallocs, image and pixmap data, JIT and toolkit buffers. Blocks "
            "over 8 MB are listed on their own.");
    case MappingGroup::ThreadStacks:
        return QStringLiteral(
            "One stack per live thread. Grows with agent runs, git pumps and "
            "the thread pool; each is reserved lazily, so resident is far "
            "under the reservation.");
    case MappingGroup::MainStack:
        return QStringLiteral("The GUI thread's stack.");
    case MappingGroup::SharedMemory:
        return QStringLiteral(
            "Buffers shared with another process: the compositor's window "
            "surfaces, shared pixmaps, memfds. Counted in resident but paid "
            "for once between the sharers.");
    case MappingGroup::Graphics:
        return QStringLiteral(
            "The GL or Vulkan driver's own mappings, including anything it "
            "keeps on the card's behalf.");
    case MappingGroup::Fonts:
        return QStringLiteral(
            "Font files the toolkit mapped. File-backed and clean, so this is "
            "cache the kernel can drop under pressure.");
    case MappingGroup::Libraries:
        return QStringLiteral(
            "Code and data of every loaded .so. Text is shared with every "
            "other process using the same library; only the data segments are "
            "this process's own.");
    case MappingGroup::Executable:
        return QStringLiteral("This binary's own text and data.");
    case MappingGroup::MappedFiles:
        return QStringLiteral(
            "Other files mapped into the process: resources, locale and icon "
            "caches, anything read through mmap.");
    case MappingGroup::Reserved:
        return QStringLiteral(
            "Address space reserved with no access: guard pages and arenas "
            "not yet touched. Costs virtual size, not memory.");
    case MappingGroup::Kernel:
        return QStringLiteral("[vdso] and friends, mapped by the kernel.");
    case MappingGroup::GroupCount:
        break;
    }
    return QString();
}

MappingGroup classifyMapping(const QString &rawPath, bool writable, bool executable,
                             const QString &executablePath)
{
    Q_UNUSED(executable);
    const QString path = stripDeleted(rawPath);
    if (path.isEmpty())
        return writable ? MappingGroup::Anonymous : MappingGroup::Reserved;
    if (path.startsWith(QLatin1Char('['))) {
        if (path.startsWith(QLatin1String("[heap")))
            return MappingGroup::Heap;
        if (path.startsWith(QLatin1String("[stack")))
            return MappingGroup::MainStack;
        if (path.startsWith(QLatin1String("[anon_shmem")))
            return MappingGroup::SharedMemory;
        if (path.startsWith(QLatin1String("[anon"))) {
            // Kernels since 5.17 carry the name glibc gives a pthread stack.
            if (path.contains(QLatin1String("stack")))
                return MappingGroup::ThreadStacks;
            return MappingGroup::Anonymous;
        }
        return MappingGroup::Kernel;
    }
    if (!executablePath.isEmpty() && path == executablePath)
        return MappingGroup::Executable;
    if (path.startsWith(QLatin1String("/memfd:")) ||
        path.startsWith(QLatin1String("/dev/shm")) ||
        path.startsWith(QLatin1String("/SYSV")) ||
        path.startsWith(QLatin1String("/dev/zero")) ||
        path.startsWith(QLatin1String("/drm mm object")) ||
        path.startsWith(QLatin1String("anon_inode")))
        return MappingGroup::SharedMemory;
    if (path.startsWith(QLatin1String("/dev/dri")) ||
        path.startsWith(QLatin1String("/dev/nvidia")) ||
        path.contains(QLatin1String("nvidia")))
        return MappingGroup::Graphics;
    if (path.contains(QLatin1String("/fonts/")) ||
        hasAnySuffix(path, {".ttf", ".otf", ".ttc", ".pfb", ".pfa", ".woff",
                            ".woff2"}))
        return MappingGroup::Fonts;
    if (path.endsWith(QLatin1String(".so")) ||
        path.contains(QLatin1String(".so.")) ||
        path.endsWith(QLatin1String(".dylib")))
        return MappingGroup::Libraries;
    return MappingGroup::MappedFiles;
}

QVector<MappingSample> parseSmaps(const QByteArray &text,
                                  const QString &executablePath)
{
    QVector<MappingSample> out;
    MappingSample current;
    bool open = false;
    quint64 start = 0;
    const auto flush = [&] {
        if (open)
            out.append(current);
        open = false;
    };
    const QList<QByteArray> lines = text.split('\n');
    for (const QByteArray &line : lines) {
        if (line.isEmpty())
            continue;
        QString permissions;
        QString path;
        quint64 begin = 0;
        if (isMappingHeader(line, &begin, &permissions, &path)) {
            flush();
            current = MappingSample();
            start = begin;
            const bool writable = permissions.contains(QLatin1Char('w'));
            const bool executableBit = permissions.contains(QLatin1Char('x'));
            current.group =
                classifyMapping(path, writable, executableBit, executablePath);
            const QString clean = stripDeleted(path);
            const QString address =
                QStringLiteral("0x") + QString::number(start, 16);
            if (clean.isEmpty()) {
                current.label = current.group == MappingGroup::Reserved
                                    ? QStringLiteral("reserved")
                                    : QStringLiteral("anonymous");
                current.detail = address;
            } else if (clean.startsWith(QLatin1Char('['))) {
                current.label = clean;
                current.detail = address;
            } else {
                current.label = QFileInfo(clean).fileName();
                current.detail = path;
            }
            open = true;
            continue;
        }
        if (!open)
            continue;
        qint64 value = 0;
        if (kilobyteLine(line, "Size", &value)) {
            current.virtualBytes = value;
        } else if (kilobyteLine(line, "Rss", &value)) {
            current.residentBytes = value;
        } else if (kilobyteLine(line, "Pss", &value)) {
            current.proportionalBytes = value;
        } else if (kilobyteLine(line, "Private_Clean", &value)) {
            current.privateBytes += value;
        } else if (kilobyteLine(line, "Private_Dirty", &value)) {
            current.privateBytes += value;
            current.privateDirtyBytes = value;
        } else if (kilobyteLine(line, "Shared_Clean", &value)) {
            current.sharedBytes += value;
        } else if (kilobyteLine(line, "Shared_Dirty", &value)) {
            current.sharedBytes += value;
        } else if (kilobyteLine(line, "Swap", &value)) {
            current.swapBytes = value;
        }
    }
    flush();

    // A big anonymous block keeps its address as its label so it stays one row
    // across samples and its growth can be followed.
    for (MappingSample &sample : out) {
        const bool anonymousish = sample.group == MappingGroup::Anonymous ||
                                  sample.group == MappingGroup::ThreadStacks;
        if (anonymousish && sample.residentBytes >= kNamedAnonymousBytes)
            sample.label = sample.label + QLatin1Char(' ') + sample.detail;
    }
    return out;
}

QVector<MappingGroupSample> groupMappings(const QVector<MappingSample> &samples)
{
    // Named first: `T groups(int(MappingGroup::GroupCount));` is the most
    // vexing parse, which clang (macOS builds) reads as a function declaration.
    const int groupCount = int(MappingGroup::GroupCount);
    QVector<MappingGroupSample> groups(groupCount);
    QVector<QHash<QString, int>> byLabel(groupCount);
    for (int i = 0; i < groups.size(); ++i)
        groups[i].group = MappingGroup(i);

    for (const MappingSample &sample : samples) {
        const int g = int(sample.group);
        if (g < 0 || g >= groups.size())
            continue;
        MappingGroupSample &group = groups[g];
        group.virtualBytes += sample.virtualBytes;
        group.residentBytes += sample.residentBytes;
        group.proportionalBytes += sample.proportionalBytes;
        group.privateBytes += sample.privateBytes;
        group.privateDirtyBytes += sample.privateDirtyBytes;
        group.sharedBytes += sample.sharedBytes;
        group.swapBytes += sample.swapBytes;
        group.mappings += 1;

        const auto found = byLabel[g].constFind(sample.label);
        if (found == byLabel[g].constEnd()) {
            byLabel[g].insert(sample.label, group.items.size());
            group.items.append(sample);
            continue;
        }
        MappingSample &item = group.items[found.value()];
        item.virtualBytes += sample.virtualBytes;
        item.residentBytes += sample.residentBytes;
        item.proportionalBytes += sample.proportionalBytes;
        item.privateBytes += sample.privateBytes;
        item.privateDirtyBytes += sample.privateDirtyBytes;
        item.sharedBytes += sample.sharedBytes;
        item.swapBytes += sample.swapBytes;
        item.mappings += 1;
        if (item.mappings == 2 && !item.detail.startsWith(QLatin1Char('/')))
            item.detail.clear(); // addresses stop meaning anything once merged
    }

    QVector<MappingGroupSample> out;
    for (MappingGroupSample &group : groups) {
        if (group.mappings == 0)
            continue;
        std::sort(group.items.begin(), group.items.end(),
                  [](const MappingSample &a, const MappingSample &b) {
                      if (a.residentBytes != b.residentBytes)
                          return a.residentBytes > b.residentBytes;
                      return a.label < b.label;
                  });
        out.append(group);
    }
    return out;
}

AllocatorSample sampleAllocator()
{
    AllocatorSample out;
#if defined(Q_OS_LINUX) && defined(__GLIBC__) &&                               \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    // mallinfo2 uses size_t fields; the older mallinfo() wraps past 2 GB,
    // which is exactly the size this page exists to look at.
    const struct mallinfo2 info = mallinfo2();
    out.valid = true;
    out.arenaBytes = qint64(info.arena);
    out.mmapBytes = qint64(info.hblkhd);
    out.inUseBytes = qint64(info.uordblks);
    out.freeBytes = qint64(info.fordblks);
    out.releasableBytes = qint64(info.keepcost);
    out.mmapChunks = int(info.hblks);
    out.freeChunks = int(info.ordblks);
#endif
    return out;
}

namespace {

struct ProbeEntry {
    QString group;
    QString name;
    std::function<ProbeSample()> read;
};

QMutex &probeMutex()
{
    static QMutex mutex;
    return mutex;
}

QVector<ProbeEntry> &probeList()
{
    static QVector<ProbeEntry> probes;
    return probes;
}

} // namespace

void registerProbe(const QString &group, const QString &name,
                   std::function<ProbeSample()> read)
{
    if (!read)
        return;
    QMutexLocker lock(&probeMutex());
    for (ProbeEntry &entry : probeList()) {
        if (entry.group == group && entry.name == name) {
            entry.read = std::move(read);
            return;
        }
    }
    probeList().append(ProbeEntry{group, name, std::move(read)});
}

QVector<ProbeSample> sampleProbes()
{
    QVector<ProbeEntry> entries;
    {
        QMutexLocker lock(&probeMutex());
        entries = probeList();
    }
    QVector<ProbeSample> out;
    out.reserve(entries.size());
    for (const ProbeEntry &entry : entries) {
        ProbeSample sample = entry.read();
        if (sample.bytes < 0)
            continue; // the subsystem is not up yet; do not show a zero
        if (sample.name.isEmpty())
            sample.name = entry.name;
        if (sample.group.isEmpty())
            sample.group = entry.group;
        out.append(sample);
    }
    std::sort(out.begin(), out.end(),
              [](const ProbeSample &a, const ProbeSample &b) {
                  if (a.group != b.group)
                      return a.group < b.group;
                  return a.bytes > b.bytes;
              });
    return out;
}

void clearProbes()
{
    QMutexLocker lock(&probeMutex());
    probeList().clear();
}

qint64 ProcessMemorySnapshot::groupResident(MappingGroup group) const
{
    for (const MappingGroupSample &sample : groups) {
        if (sample.group == group)
            return sample.residentBytes;
    }
    return 0;
}

void parseStatusMemory(const QByteArray &text, ProcessMemorySnapshot *out)
{
    if (!out)
        return;
    const auto field = [&text](const char *key) {
        const qint64 value = parseKilobyteField(text, key);
        return value < 0 ? 0 : value;
    };
    out->virtualBytes = field("VmSize");
    out->peakVirtualBytes = field("VmPeak");
    out->residentBytes = field("VmRSS");
    out->peakResidentBytes = field("VmHWM");
    out->anonResidentBytes = field("RssAnon");
    out->fileResidentBytes = field("RssFile");
    out->shmemResidentBytes = field("RssShmem");
    out->dataBytes = field("VmData");
    out->stackBytes = field("VmStk");
    out->textBytes = field("VmExe");
    out->pageTableBytes = field("VmPTE");
    out->swapBytes = field("VmSwap");
    // Threads is a plain count, not a kB line.
    for (const QByteArray &line : text.split('\n')) {
        if (!line.startsWith("Threads:"))
            continue;
        out->threads = line.mid(8).trimmed().toInt();
        break;
    }
}

ProcessMemorySnapshot sampleProcessMemory(bool includeMappings)
{
    ProcessMemorySnapshot snapshot;
    QElapsedTimer timer;
    timer.start();
    snapshot.sampledAtMs = QDateTime::currentMSecsSinceEpoch();
    snapshot.machineTotalBytes = SystemStats::totalMemoryBytes();
    snapshot.machineAvailableBytes = SystemStats::availableMemoryBytes();
    snapshot.allocator = sampleAllocator();
#ifdef Q_OS_LINUX
    const QByteArray status = readProcFile(QStringLiteral("/proc/self/status"));
    if (!status.isEmpty()) {
        parseStatusMemory(status, &snapshot);
        snapshot.valid = true;
    }
    if (includeMappings) {
        const QString exe =
            QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
        const QVector<MappingSample> mappings =
            parseSmaps(readProcFile(QStringLiteral("/proc/self/smaps")), exe);
        snapshot.groups = groupMappings(mappings);
        for (const MappingGroupSample &group : snapshot.groups) {
            snapshot.proportionalBytes += group.proportionalBytes;
            snapshot.privateBytes += group.privateBytes;
        }
    }
#else
    snapshot.residentBytes = SystemStats::residentBytes();
    snapshot.valid = snapshot.residentBytes > 0;
    Q_UNUSED(includeMappings);
#endif
    snapshot.sampleCostUs = timer.nsecsElapsed() / 1000;
    return snapshot;
}

} // namespace memory
} // namespace forkmesh
