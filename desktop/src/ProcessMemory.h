#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <functional>

// Where this process's memory actually is, read from the kernel rather than
// estimated: every mapping in /proc/self/maps with its resident, proportional,
// private and swapped pages from /proc/self/smaps, the process totals from
// /proc/self/status, and the allocator's own view from mallinfo2().
//
// Why a whole page for it: "the app uses 1.4 GB" is not actionable. A leak in
// a pixmap cache, a font file the toolkit mapped once, a thread stack per
// agent run and a heap that malloc never returned to the kernel all read the
// same on the total, and each wants a different fix. The Memory page shows the
// breakdown live, so growth can be watched happening and attributed while it
// happens rather than reconstructed afterwards from a stall record.
//
// Everything here is Linux-specific and degrades to an invalid snapshot
// elsewhere; the page says so rather than inventing numbers.
namespace forkmesh {
namespace memory {

// Buckets a mapping is sorted into. The page paints one row per group with its
// mappings underneath, and the chart stacks the groups in this order, so the
// order is part of the contract: it runs from the memory the app allocates
// itself down to the memory the loader and the kernel gave it.
enum class MappingGroup {
    Heap,          // [heap] - what brk() grew for malloc
    Anonymous,     // private writable mappings: big mallocs, Qt image data
    ThreadStacks,  // pthread stacks, named by glibc on new enough kernels
    MainStack,     // [stack]
    SharedMemory,  // memfd, /dev/shm, SysV: compositor buffers, shm images
    Graphics,      // /dev/dri, nvidia, vulkan / GL driver mappings
    Fonts,         // font files the toolkit mapped
    Libraries,     // .so text and data
    Executable,    // this binary
    MappedFiles,   // every other file-backed mapping
    Reserved,      // PROT_NONE guard and reservation space
    Kernel,        // [vdso], [vvar], [vsyscall]
    GroupCount
};

QString groupTitle(MappingGroup group);
// One line saying what lands in the bucket and why it grows; the page shows it
// as the group row's tooltip.
QString groupDescription(MappingGroup group);

// One line of /proc/self/smaps, or several merged: mappings that share a label
// (a library's four segments, a thousand anonymous chunks) are added together
// and carry how many were folded in.
struct MappingSample {
    QString label;   // what the page shows: basename, "[heap]", "anonymous"
    QString detail;  // full path or address, for the tooltip
    MappingGroup group = MappingGroup::Anonymous;
    qint64 virtualBytes = 0;
    qint64 residentBytes = 0;
    qint64 proportionalBytes = 0; // Pss: shared pages divided by sharers
    qint64 privateBytes = 0;      // Private_Clean + Private_Dirty
    qint64 privateDirtyBytes = 0; // the part that can only go to swap
    qint64 sharedBytes = 0;
    qint64 swapBytes = 0;
    int mappings = 1;
};

// A group with its mappings, already totalled and sorted.
struct MappingGroupSample {
    MappingGroup group = MappingGroup::Anonymous;
    qint64 virtualBytes = 0;
    qint64 residentBytes = 0;
    qint64 proportionalBytes = 0;
    qint64 privateBytes = 0;
    qint64 privateDirtyBytes = 0;
    qint64 sharedBytes = 0;
    qint64 swapBytes = 0;
    int mappings = 0;
    QVector<MappingSample> items;
};

// glibc's malloc arenas. The gap between what the app asked for (inUseBytes)
// and what the process holds (arenaBytes + mmapBytes) is memory that was freed
// but never returned to the kernel - it reads as a leak on the RSS total and
// is not one.
struct AllocatorSample {
    bool valid = false;
    qint64 arenaBytes = 0;   // brk heaps, total
    qint64 mmapBytes = 0;    // chunks malloc took straight from mmap
    qint64 inUseBytes = 0;   // handed out and not yet freed
    qint64 freeBytes = 0;    // freed, still held by the allocator
    qint64 releasableBytes = 0; // the top-of-heap trim malloc_trim() could give back
    int mmapChunks = 0;
    int freeChunks = 0;
};

// A subsystem's own count of what it is holding: the favicon cache's pixmaps,
// the log buffer's lines. The kernel can only say "anonymous memory"; these
// probes are how the page names the parts of that the app knows about.
struct ProbeSample {
    QString name;
    QString group;   // "Caches", "Documents", ...: a heading on the page
    QString detail;  // "312 pixmaps", "41k lines"
    qint64 bytes = 0;
    bool estimated = true; // container overhead is never exact; say so
};

struct ProcessMemorySnapshot {
    bool valid = false;
    qint64 sampledAtMs = 0;
    qint64 sampleCostUs = 0; // how long the read itself took

    // /proc/self/status
    qint64 virtualBytes = 0;
    qint64 residentBytes = 0;
    qint64 peakResidentBytes = 0;
    qint64 peakVirtualBytes = 0;
    qint64 anonResidentBytes = 0;
    qint64 fileResidentBytes = 0;
    qint64 shmemResidentBytes = 0;
    qint64 dataBytes = 0;
    qint64 stackBytes = 0;
    qint64 textBytes = 0;
    qint64 pageTableBytes = 0;
    qint64 swapBytes = 0;
    int threads = 0;

    // Summed from smaps: proportional set size, and the part of RSS no other
    // process shares (the memory that would actually come back on exit).
    qint64 proportionalBytes = 0;
    qint64 privateBytes = 0;

    // Machine context, so a number can be read as a share of the box.
    qint64 machineTotalBytes = 0;
    qint64 machineAvailableBytes = 0;

    AllocatorSample allocator;
    QVector<MappingGroupSample> groups; // in MappingGroup order, empty ones dropped
    QVector<ProbeSample> probes;

    qint64 groupResident(MappingGroup group) const;
};

// Pure parsers, shared with the tests. Each tolerates truncated or foreign
// input: unknown keys are ignored and a malformed mapping header ends the
// current mapping rather than corrupting the next one.
void parseStatusMemory(const QByteArray &text, ProcessMemorySnapshot *out);
QVector<MappingSample> parseSmaps(const QByteArray &text,
                                  const QString &executablePath);
// Merge by label, total, and sort groups and their mappings by resident size.
QVector<MappingGroupSample> groupMappings(const QVector<MappingSample> &samples);
// Which bucket a mapping path belongs in. `path` is the smaps tail, possibly
// empty (anonymous) or bracketed ("[heap]").
MappingGroup classifyMapping(const QString &path, bool writable, bool executable,
                             const QString &executablePath);

AllocatorSample sampleAllocator();

// Full read: status, smaps, mallinfo2 and every registered probe. Costs a few
// milliseconds on a process with thousands of mappings, so the page samples on
// a worker thread; probes must be gathered on the thread that owns the data.
ProcessMemorySnapshot sampleProcessMemory(bool includeMappings = true);

// Probes are registered once at startup and read on the GUI thread. A probe
// that cannot answer returns a negative byte count and is dropped.
void registerProbe(const QString &group, const QString &name,
                   std::function<ProbeSample()> read);
QVector<ProbeSample> sampleProbes();
void clearProbes(); // tests

} // namespace memory
} // namespace forkmesh
