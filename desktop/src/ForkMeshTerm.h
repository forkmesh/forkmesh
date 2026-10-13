#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace forkmesh::term {

inline constexpr int kProtocolVersion = 1;
inline constexpr std::size_t kMaxTextBytes = 64 * 1024;
inline constexpr std::size_t kMaxFromBytes = 128;
inline constexpr std::size_t kMaxHeaderBytes = 96;
inline constexpr int kMaxTypedPerWindow = 20;
inline constexpr int kMaxKeysPerWindow = 30;
inline constexpr std::int64_t kRateWindowMs = 60 * 1000;
inline constexpr std::int64_t kUserTypingQuietMs = 1500;
inline constexpr std::int64_t kEnterDelayMs = 120;
inline constexpr unsigned char kToggleByte = 0x1e;

struct Options {
    bool remote = true;
    bool help = false;
    bool version = false;
    std::vector<std::string> argv;
};

bool parseArgs(const std::vector<std::string> &args, Options *options, std::string *error);
std::string usage();

std::string sanitizeText(const std::string &text);
std::string sanitizeLabel(const std::string &label);
std::string pasteBytes(const std::string &text, bool bracketed);
std::string enterBytes();
std::string keyBytes(const std::string &key, const std::string &cli);
std::string interruptKeyName(const std::string &cli);
std::string cliName(const std::string &argv0);
std::string noticeLine(const std::string &what, const std::string &from);
std::string jsonEscape(const std::string &text);

class PasteModeTracker {
public:
    void feed(const char *data, std::size_t size);
    bool enabled() const { return m_enabled; }

private:
    void finish(char final);
    int m_state = 0;
    std::string m_params;
    bool m_enabled = false;
};

class RateLimiter {
public:
    RateLimiter(int max, std::int64_t windowMs) : m_max(max), m_windowMs(windowMs) {}
    bool allow(std::int64_t nowMs);

private:
    int m_max;
    std::int64_t m_windowMs;
    std::deque<std::int64_t> m_times;
};

struct Request {
    std::string verb;
    std::string from;
    std::string payload;
};

enum class Parse { Incomplete, Ok, Bad, TooLong };
Parse parseRequest(const std::string &buffer, Request *request, std::size_t *consumed);
std::string requestFrame(const std::string &verb, const std::string &from, const std::string &payload);

struct Registration {
    long long pid = 0;
    std::string start;
    long long child = 0;
    std::string cli;
    std::string cwd;
    std::string socket;
    bool remote = true;
    long long startedMs = 0;
    std::string tty;
};
std::string registrationJson(const Registration &registration);

std::string runtimeDir();
bool ensurePrivateDir(const std::string &path, std::string *error);
std::string procStart(long long pid);

int run(const Options &options);
int main(int argc, char **argv);

} // namespace forkmesh::term
