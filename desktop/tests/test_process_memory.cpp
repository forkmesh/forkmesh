// The Memory page reads /proc/self/smaps and /proc/self/status through these
// parsers. The fixtures are verbatim shapes from the kernel so a change to the
// classifier or the merge shows up here rather than as a wrong row on screen.
#include "../src/MemoryUsageChart.h"
#include "../src/ProcessMemory.h"

#include <QApplication>
#include <QByteArray>
#include <QString>

#include <cstdio>
#include <vector>

using namespace forkmesh::memory;

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

void checkEq(qint64 got, qint64 want, const char *what)
{
    if (got != want) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s: got %lld want %lld\n", what,
                     static_cast<long long>(got), static_cast<long long>(want));
    }
}

QByteArray mapping(const char *header, int sizeKb, int rssKb, int pssKb,
                   int sharedCleanKb, int privateCleanKb, int privateDirtyKb,
                   int swapKb = 0)
{
    QByteArray out(header);
    out += '\n';
    out += "Size:           " + QByteArray::number(sizeKb) + " kB\n";
    out += "KernelPageSize:        4 kB\n";
    out += "MMUPageSize:           4 kB\n";
    out += "Rss:            " + QByteArray::number(rssKb) + " kB\n";
    out += "Pss:            " + QByteArray::number(pssKb) + " kB\n";
    out += "Pss_Dirty:      " + QByteArray::number(privateDirtyKb) + " kB\n";
    out += "Shared_Clean:   " + QByteArray::number(sharedCleanKb) + " kB\n";
    out += "Shared_Dirty:          0 kB\n";
    out += "Private_Clean:  " + QByteArray::number(privateCleanKb) + " kB\n";
    out += "Private_Dirty:  " + QByteArray::number(privateDirtyKb) + " kB\n";
    out += "Referenced:     " + QByteArray::number(rssKb) + " kB\n";
    out += "Anonymous:             0 kB\n";
    out += "Swap:           " + QByteArray::number(swapKb) + " kB\n";
    out += "SwapPss:               0 kB\n";
    out += "Locked:                0 kB\n";
    out += "THPeligible:           0\n";
    out += "VmFlags: rd mr mw me sd\n";
    return out;
}

const QString kExe = QStringLiteral("/opt/forkmesh/bin/forkmesh");

QByteArray fixture()
{
    QByteArray text;
    text += mapping("55beb4929000-55beb4b29000 r-xp 00000000 08:02 9307616                    /opt/forkmesh/bin/forkmesh",
                    2048, 1024, 1024, 0, 1024, 0);
    text += mapping("55beb6000000-55beb7000000 rw-p 00000000 00:00 0                          [heap]",
                    16384, 12000, 12000, 0, 0, 12000, 100);
    text += mapping("7f2c10000000-7f2c10800000 rw-p 00000000 00:00 0 ",
                    8192, 64, 64, 0, 0, 64);
    text += mapping("7f2c11000000-7f2c11800000 rw-p 00000000 00:00 0 ",
                    8192, 32, 32, 0, 0, 32);
    // A 20 MB decoded image: gets a row of its own.
    text += mapping("7f2c20000000-7f2c21400000 rw-p 00000000 00:00 0 ",
                    20480, 20480, 20480, 0, 0, 20480);
    text += mapping("7f2c30000000-7f2c30001000 ---p 00000000 00:00 0 ",
                    4, 0, 0, 0, 0, 0);
    text += mapping("7f2c31000000-7f2c31800000 rw-p 00000000 00:00 0                          [anon:glibc: pthread stack: 12345]",
                    8192, 40, 40, 0, 0, 40);
    // libc's segments merge into one row with four mappings.
    text += mapping("7f2c40000000-7f2c40028000 r--p 00000000 08:02 100                        /usr/lib/x86_64-linux-gnu/libc.so.6",
                    160, 160, 20, 160, 0, 0);
    text += mapping("7f2c40028000-7f2c401bd000 r-xp 00028000 08:02 100                        /usr/lib/x86_64-linux-gnu/libc.so.6",
                    1620, 1200, 60, 1200, 0, 0);
    text += mapping("7f2c401bd000-7f2c40215000 r--p 001bd000 08:02 100                        /usr/lib/x86_64-linux-gnu/libc.so.6",
                    352, 64, 8, 64, 0, 0);
    text += mapping("7f2c40215000-7f2c40219000 rw-p 00214000 08:02 100                        /usr/lib/x86_64-linux-gnu/libc.so.6",
                    16, 16, 16, 0, 0, 16);
    text += mapping("7f2c50000000-7f2c50100000 r--s 00000000 08:02 200                        /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                    1024, 700, 350, 700, 0, 0);
    text += mapping("7f2c60000000-7f2c60400000 rw-s 00000000 00:01 300                        /memfd:wayland-shm (deleted)",
                    4096, 4096, 2048, 0, 0, 4096);
    text += mapping("7f2c70000000-7f2c70001000 r--p 00000000 08:02 400                        /home/me/My Files/cache.bin",
                    4, 4, 4, 0, 4, 0);
    text += mapping("7ffc00000000-7ffc00021000 rw-p 00000000 00:00 0                          [stack]",
                    132, 40, 40, 0, 0, 40);
    text += mapping("7ffc00100000-7ffc00102000 r-xp 00000000 00:00 0                          [vdso]",
                    8, 4, 0, 4, 0, 0);
    return text;
}

const MappingGroupSample *findGroup(const QVector<MappingGroupSample> &groups,
                                    MappingGroup group)
{
    for (const MappingGroupSample &g : groups) {
        if (g.group == group)
            return &g;
    }
    return nullptr;
}

void testClassify()
{
    check(classifyMapping(QString(), true, false, kExe) == MappingGroup::Anonymous,
          "writable anonymous");
    check(classifyMapping(QString(), false, false, kExe) == MappingGroup::Reserved,
          "PROT_NONE anonymous is reserved");
    check(classifyMapping(QStringLiteral("[heap]"), true, false, kExe) ==
              MappingGroup::Heap,
          "[heap]");
    check(classifyMapping(QStringLiteral("[stack]"), true, false, kExe) ==
              MappingGroup::MainStack,
          "[stack]");
    check(classifyMapping(QStringLiteral("[anon:glibc: pthread stack: 9]"), true,
                          false, kExe) == MappingGroup::ThreadStacks,
          "named pthread stack");
    check(classifyMapping(QStringLiteral("[anon:scudo:primary]"), true, false,
                          kExe) == MappingGroup::Anonymous,
          "other named anonymous");
    check(classifyMapping(QStringLiteral("[vvar]"), false, false, kExe) ==
              MappingGroup::Kernel,
          "[vvar]");
    check(classifyMapping(kExe, false, true, kExe) == MappingGroup::Executable,
          "own binary");
    check(classifyMapping(kExe + QStringLiteral(" (deleted)"), false, true, kExe) ==
              MappingGroup::Executable,
          "own binary replaced on disk by a rebuild");
    check(classifyMapping(QStringLiteral("/usr/lib/libQt6Core.so.6.8.2"), false,
                          true, kExe) == MappingGroup::Libraries,
          "versioned .so");
    check(classifyMapping(QStringLiteral("/memfd:xshmfence (deleted)"), true,
                          false, kExe) == MappingGroup::SharedMemory,
          "memfd");
    check(classifyMapping(QStringLiteral("/dev/dri/renderD128"), true, false,
                          kExe) == MappingGroup::Graphics,
          "render node");
    check(classifyMapping(QStringLiteral("/usr/share/fonts/noto/Noto.otf"), false,
                          false, kExe) == MappingGroup::Fonts,
          "font");
    check(classifyMapping(QStringLiteral("/usr/lib/locale/locale-archive"), false,
                          false, kExe) == MappingGroup::MappedFiles,
          "other file");
}

void testParseAndGroup()
{
    const QVector<MappingSample> samples = parseSmaps(fixture(), kExe);
    checkEq(samples.size(), 16, "one sample per mapping");

    bool sawSpacePath = false;
    for (const MappingSample &s : samples) {
        if (s.detail == QStringLiteral("/home/me/My Files/cache.bin"))
            sawSpacePath = s.label == QStringLiteral("cache.bin");
    }
    check(sawSpacePath, "a path with a space survives whole");

    const QVector<MappingGroupSample> groups = groupMappings(samples);
    // Enum order, empty groups dropped.
    for (int i = 1; i < groups.size(); ++i)
        check(int(groups.at(i - 1).group) < int(groups.at(i).group),
              "groups stay in enum order");

    const MappingGroupSample *heap = findGroup(groups, MappingGroup::Heap);
    check(heap != nullptr, "heap group present");
    if (heap) {
        checkEq(heap->residentBytes, 12000 * 1024, "heap resident");
        checkEq(heap->privateDirtyBytes, 12000 * 1024, "heap private dirty");
        checkEq(heap->swapBytes, 100 * 1024, "heap swap");
    }

    const MappingGroupSample *anon = findGroup(groups, MappingGroup::Anonymous);
    check(anon != nullptr, "anonymous group present");
    if (anon) {
        checkEq(anon->mappings, 3, "three anonymous mappings");
        checkEq(anon->items.size(), 2, "large block alone, small ones merged");
        if (anon->items.size() == 2) {
            // Sorted by resident: the 20 MB block first, named by its address.
            check(anon->items.at(0).label ==
                      QStringLiteral("anonymous 0x7f2c20000000"),
                  "large anonymous block keeps its address");
            checkEq(anon->items.at(0).residentBytes, 20480 * 1024,
                    "large block resident");
            check(anon->items.at(1).label == QStringLiteral("anonymous"),
                  "small blocks merged");
            checkEq(anon->items.at(1).mappings, 2, "merged count");
            checkEq(anon->items.at(1).residentBytes, 96 * 1024, "merged resident");
            check(anon->items.at(1).detail.isEmpty(),
                  "merged anonymous rows drop the single address");
        }
    }

    const MappingGroupSample *libs = findGroup(groups, MappingGroup::Libraries);
    check(libs != nullptr, "libraries present");
    if (libs && !libs->items.isEmpty()) {
        const MappingSample &libc = libs->items.first();
        check(libc.label == QStringLiteral("libc.so.6"), "libc label is basename");
        checkEq(libc.mappings, 4, "libc segments merged");
        checkEq(libc.residentBytes, (160 + 1200 + 64 + 16) * 1024, "libc resident");
        checkEq(libc.proportionalBytes, (20 + 60 + 8 + 16) * 1024, "libc pss");
        checkEq(libc.sharedBytes, (160 + 1200 + 64) * 1024, "libc shared");
        checkEq(libc.privateBytes, 16 * 1024, "libc private");
        check(libc.detail.startsWith(QLatin1Char('/')),
              "merged file rows keep their path");
    }

    check(findGroup(groups, MappingGroup::ThreadStacks) != nullptr, "thread stack");
    check(findGroup(groups, MappingGroup::MainStack) != nullptr, "main stack");
    check(findGroup(groups, MappingGroup::Reserved) != nullptr, "reserved");
    check(findGroup(groups, MappingGroup::Fonts) != nullptr, "fonts");
    check(findGroup(groups, MappingGroup::SharedMemory) != nullptr, "memfd");
    check(findGroup(groups, MappingGroup::Executable) != nullptr, "executable");
    check(findGroup(groups, MappingGroup::Kernel) != nullptr, "vdso");
    check(findGroup(groups, MappingGroup::MappedFiles) != nullptr, "other file");
    check(findGroup(groups, MappingGroup::Graphics) == nullptr,
          "no empty group rows");

    // Truncated input (the read raced a munmap) must not invent a mapping.
    const QByteArray cut = fixture().left(fixture().indexOf("[heap]") + 10);
    const QVector<MappingSample> partial = parseSmaps(cut, kExe);
    checkEq(partial.size(), 2, "truncated read keeps the complete headers only");
    check(parseSmaps(QByteArray("garbage\nnot: smaps\n"), kExe).isEmpty(),
          "foreign text parses to nothing");
}

void testStatus()
{
    const QByteArray status =
        "Name:\tforkmesh\n"
        "Threads:\t57\n"
        "VmPeak:\t 4282944 kB\n"
        "VmSize:\t 4182944 kB\n"
        "VmHWM:\t  958572 kB\n"
        "VmRSS:\t  858572 kB\n"
        "RssAnon:\t  600000 kB\n"
        "RssFile:\t  250000 kB\n"
        "RssShmem:\t    8572 kB\n"
        "VmData:\t 1193448 kB\n"
        "VmStk:\t     136 kB\n"
        "VmExe:\t   60312 kB\n"
        "VmPTE:\t    3112 kB\n"
        "VmSwap:\t   12288 kB\n";
    ProcessMemorySnapshot s;
    parseStatusMemory(status, &s);
    checkEq(s.residentBytes, 858572LL * 1024, "VmRSS");
    checkEq(s.peakResidentBytes, 958572LL * 1024, "VmHWM");
    checkEq(s.virtualBytes, 4182944LL * 1024, "VmSize");
    checkEq(s.anonResidentBytes, 600000LL * 1024, "RssAnon");
    checkEq(s.swapBytes, 12288LL * 1024, "VmSwap");
    checkEq(s.pageTableBytes, 3112LL * 1024, "VmPTE");
    checkEq(s.threads, 57, "Threads is a count, not kB");
    ProcessMemorySnapshot empty;
    parseStatusMemory(QByteArray(), &empty);
    checkEq(empty.residentBytes, 0, "missing keys read as zero");
}

void testNiceCeiling()
{
    const qint64 mb = 1024 * 1024;
    checkEq(MemoryUsageChart::niceCeiling(0), mb, "empty chart scale");
    checkEq(MemoryUsageChart::niceCeiling(mb), mb, "exact");
    checkEq(MemoryUsageChart::niceCeiling(mb + 1), 2 * mb, "1 -> 2");
    checkEq(MemoryUsageChart::niceCeiling(3 * mb), 5 * mb, "2 -> 5");
    checkEq(MemoryUsageChart::niceCeiling(7 * mb), 10 * mb, "5 -> 10");
    checkEq(MemoryUsageChart::niceCeiling(310 * mb), 500 * mb, "hundreds");
    checkEq(MemoryUsageChart::niceCeiling(1400 * mb), 2000 * mb, "gigabytes");
}

void testChartWindow()
{
    MemoryUsageChart chart;
    chart.setWindowMs(60 * 1000);
    for (int i = 0; i < 120; ++i)
        chart.appendPoint({qint64(i) * 1000, {qint64(i), 1}});
    // 60 s window at 1 s spacing keeps the points inside it, plus the edge.
    check(chart.pointCount() <= 62 && chart.pointCount() >= 60,
          "old points fall out of the window");
    checkEq(chart.peakTotal(), 120, "peak over the window");
}

void testProbes()
{
    clearProbes();
    registerProbe(QStringLiteral("Caches"), QStringLiteral("small"), [] {
        ProbeSample s;
        s.bytes = 10;
        return s;
    });
    registerProbe(QStringLiteral("Caches"), QStringLiteral("big"), [] {
        ProbeSample s;
        s.bytes = 1000;
        s.detail = QStringLiteral("3 items");
        return s;
    });
    registerProbe(QStringLiteral("Buffers"), QStringLiteral("not ready"), [] {
        ProbeSample s;
        s.bytes = -1;
        return s;
    });
    // Re-registering a name replaces it rather than listing it twice.
    registerProbe(QStringLiteral("Caches"), QStringLiteral("small"), [] {
        ProbeSample s;
        s.bytes = 20;
        return s;
    });
    const QVector<ProbeSample> probes = sampleProbes();
    checkEq(probes.size(), 2, "unready probe dropped, duplicate replaced");
    if (probes.size() == 2) {
        check(probes.at(0).name == QStringLiteral("big"), "largest first in group");
        check(probes.at(0).group == QStringLiteral("Caches"), "group filled in");
        checkEq(probes.at(1).bytes, 20, "replacement probe read");
    }
    clearProbes();
}

void testLiveSample()
{
#ifdef Q_OS_LINUX
    // Touch 32 MB so there is a large anonymous block to find.
    std::vector<char> block(32 * 1024 * 1024);
    for (size_t i = 0; i < block.size(); i += 4096)
        block[i] = char(i);
    const ProcessMemorySnapshot s = sampleProcessMemory();
    check(s.valid, "live sample valid on Linux");
    check(s.residentBytes > 32LL * 1024 * 1024, "live resident includes the block");
    check(!s.groups.isEmpty(), "live groups");
    check(s.threads >= 1, "live thread count");
    qint64 summed = 0;
    for (const MappingGroupSample &g : s.groups)
        summed += g.residentBytes;
    // smaps and status are read a moment apart and status is per-thread
    // cached, so allow 10 % either way.
    check(summed > s.residentBytes * 9 / 10 && summed < s.residentBytes * 11 / 10,
          "groups add up to VmRSS");
    check(s.allocator.valid, "mallinfo2 available");
    volatile char sink = block[4096];
    Q_UNUSED(sink);
#endif
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testClassify();
    testParseAndGroup();
    testStatus();
    testNiceCeiling();
    testChartWindow();
    testProbes();
    testLiveSample();
    if (g_failures == 0)
        std::printf("process memory: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
