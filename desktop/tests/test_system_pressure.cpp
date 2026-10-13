// The stall watchdog and the periodic refresh timers read the machine's
// pressure stall information through these parsers. Kept apart from the
// window suite so the /proc formats stay pinned where CI always runs.
#include "../src/SystemPressure.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

void checkNear(double got, double want, const char *what)
{
    if (got < want - 0.005 || got > want + 0.005) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s: got %f want %f\n", what, got, want);
    }
}

// Verbatim /proc/pressure/memory from the deploy box minutes after the
// 2026-09-12 13:35 freeze (load 33.9 on 36 cpus, swap in use).
const char kMemoryPsi[] =
    "some avg10=49.73 avg60=43.02 avg300=30.46 total=395895545\n"
    "full avg10=45.35 avg60=38.79 avg300=27.43 total=367337629\n";

const char kIdlePsi[] =
    "some avg10=0.00 avg60=0.00 avg300=0.00 total=163495681\n"
    "full avg10=0.00 avg60=0.00 avg300=0.00 total=153429938\n";

// Older kernels expose only the "some" line for cpu.
const char kCpuPsiSomeOnly[] =
    "some avg10=0.48 avg60=1.50 avg300=2.06 total=163495681\n";

const char kMemInfo[] =
    "MemTotal:       32756204 kB\n"
    "MemFree:        11473556 kB\n"
    "MemAvailable:   14686120 kB\n"
    "SwapTotal:      25165820 kB\n"
    "SwapFree:       24473344 kB\n";

const char kSelfStatus[] =
    "Name:\tforkmesh\n"
    "VmRSS:\t  858572 kB\n"
    "VmSwap:\t   12288 kB\n";

void testPressureParser()
{
    double some = -1, full = -1, full60 = -1;
    check(forkmesh::parsePressureStallFile(kMemoryPsi, &some, &full, &full60),
          "memory PSI parses");
    checkNear(some, 49.73, "memory some avg10");
    checkNear(full, 45.35, "memory full avg10");
    checkNear(full60, 38.79, "memory full avg60");

    some = -1;
    full = -1;
    check(forkmesh::parsePressureStallFile(kCpuPsiSomeOnly, &some, &full, nullptr),
          "cpu PSI with only a some line parses");
    checkNear(some, 0.48, "cpu some avg10");
    check(full == -1, "a missing full line leaves the output untouched");

    some = -1;
    check(!forkmesh::parsePressureStallFile(QByteArray(), &some, nullptr, nullptr),
          "an absent file parses as unavailable");
    check(some == -1, "an absent file leaves the output untouched");
    check(!forkmesh::parsePressureStallFile("garbage\n", &some, nullptr, nullptr),
          "garbage parses as unavailable");
    check(forkmesh::parsePressureStallFile(kIdlePsi, &some, &full, nullptr) &&
              some == 0.0 && full == 0.0,
          "an idle machine parses to zero pressure");
}

void testLoadAndMemoryParsers()
{
    double load = -1;
    check(forkmesh::parseLoadAverage("33.90 31.05 23.61 12/2831 610233\n", &load),
          "loadavg parses");
    checkNear(load, 33.90, "load average 1 min");
    check(!forkmesh::parseLoadAverage(QByteArray(), &load), "empty loadavg");
    check(!forkmesh::parseLoadAverage("x y z", &load), "garbage loadavg");

    check(forkmesh::parseKilobyteField(kMemInfo, "MemAvailable") ==
              14686120LL * 1024,
          "MemAvailable in bytes");
    check(forkmesh::parseKilobyteField(kMemInfo, "SwapTotal") ==
              25165820LL * 1024,
          "SwapTotal in bytes");
    check(forkmesh::parseKilobyteField(kMemInfo, "MemFree") == 11473556LL * 1024,
          "MemFree in bytes");
    check(forkmesh::parseKilobyteField(kMemInfo, "Mem") == -1,
          "a key prefix does not match a longer key");
    check(forkmesh::parseKilobyteField(kMemInfo, "Missing") == -1,
          "a missing key is -1");
    check(forkmesh::parseKilobyteField(kSelfStatus, "VmRSS") == 858572LL * 1024,
          "VmRSS with a tab separator");
    check(forkmesh::parseKilobyteField(kSelfStatus, "VmSwap") == 12288LL * 1024,
          "VmSwap with a tab separator");
}

void testPressureRuleAndDescription()
{
    forkmesh::SystemPressureSnapshot idle;
    check(!forkmesh::systemUnderPressure(idle),
          "no PSI at all never holds work back");
    idle.valid = true;
    check(!forkmesh::systemUnderPressure(idle), "zero pressure is not pressure");
    idle.memoryFullAvg10 = forkmesh::kMemoryPressureFullAvg10Percent - 0.01;
    check(!forkmesh::systemUnderPressure(idle), "just under the memory line");
    idle.memoryFullAvg10 = forkmesh::kMemoryPressureFullAvg10Percent;
    check(forkmesh::systemUnderPressure(idle), "at the memory line");

    forkmesh::SystemPressureSnapshot busyDisk;
    busyDisk.valid = true;
    busyDisk.ioFullAvg10 = forkmesh::kIoPressureFullAvg10Percent - 1.0;
    check(!forkmesh::systemUnderPressure(busyDisk),
          "a busy disk that starves nobody is not pressure");
    busyDisk.ioFullAvg10 = forkmesh::kIoPressureFullAvg10Percent + 1.0;
    check(forkmesh::systemUnderPressure(busyDisk), "io starvation is pressure");

    forkmesh::SystemPressureSnapshot thrash;
    thrash.valid = true;
    thrash.memorySomeAvg10 = 49.73;
    thrash.memoryFullAvg10 = 45.35;
    thrash.ioFullAvg10 = 0.48;
    thrash.loadAverage1 = 33.9;
    thrash.cpuCount = 36;
    thrash.memoryTotalBytes = 32756204LL * 1024;
    thrash.memoryAvailableBytes = 14686120LL * 1024;
    thrash.swapTotalBytes = 25165820LL * 1024;
    thrash.swapFreeBytes = 24473344LL * 1024;
    thrash.selfResidentBytes = 858572LL * 1024;
    thrash.selfSwappedBytes = 0;
    const QString line = forkmesh::describeSystemPressure(thrash);
    check(line.contains(QStringLiteral("memory PSI some 49.7% / full 45.4% (avg10)")),
          "description names the PSI figures");
    check(line.contains(QStringLiteral("io full 0.5%")), "description names io");
    check(line.contains(QStringLiteral("load 33.9 on 36 cpus")),
          "description names the load");
    check(line.contains(QStringLiteral("14.01 GB of 31.24 GB available")),
          "description names available memory");
    check(line.contains(QStringLiteral("swap 676.2 MB used of 24.00 GB")),
          "description names swap use");
    check(line.contains(QStringLiteral("this process 838.4 MB resident, 0 B swapped")),
          "description names the process footprint");

    forkmesh::SystemPressureSnapshot nothing;
    check(forkmesh::describeSystemPressure(nothing).isEmpty(),
          "nothing readable describes as empty");
}

void testDeferralAnnouncements()
{
    const qint64 t0 = 1'789'000'000'000LL;
    check(forkmesh::announceDeferredWork(QStringLiteral("a"), t0),
          "first deferral of a routine is announced");
    check(!forkmesh::announceDeferredWork(QStringLiteral("a"), t0 + 60'000),
          "a repeat within five minutes is quiet");
    check(forkmesh::announceDeferredWork(QStringLiteral("b"), t0 + 60'000),
          "another routine announces on its own");
    check(forkmesh::announceDeferredWork(QStringLiteral("a"),
                                         t0 + 5 * 60'000),
          "after five minutes the routine announces again");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testPressureParser();
    testLoadAndMemoryParsers();
    testPressureRuleAndDescription();
    testDeferralAnnouncements();
    // The live read must never throw or hang, whatever the host exposes.
    const forkmesh::SystemPressureSnapshot live = forkmesh::sampleSystemPressure();
    check(live.sampledAtMs > 0, "live sample is stamped");
    check(forkmesh::cachedSystemPressure().sampledAtMs >= live.sampledAtMs - 1,
          "cached sample is at least as fresh as the previous read");
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("system pressure tests passed\n");
    return 0;
}
