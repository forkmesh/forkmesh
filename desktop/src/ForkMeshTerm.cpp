#include "ForkMeshTerm.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#if __has_include("ForkMeshVersion.h")
#include "ForkMeshVersion.h"
#endif
#ifndef FORKMESH_VERSION
#define FORKMESH_VERSION "dev"
#endif

namespace forkmesh::term {

namespace {

bool validVerb(const std::string &verb)
{
    if (verb.empty() || verb.size() > 16)
        return false;
    return std::all_of(verb.begin(), verb.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; });
}

bool parseLength(const std::string &token, std::size_t *value)
{
    if (token.empty() || token.size() > 9)
        return false;
    std::size_t n = 0;
    for (char c : token) {
        if (c < '0' || c > '9')
            return false;
        n = n * 10 + std::size_t(c - '0');
    }
    *value = n;
    return true;
}

void appendUtf8(std::string &out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    } else {
        out += char(0xf0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3f));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
}

template <typename Fn>
void forEachCodePoint(const std::string &text, Fn fn)
{
    const auto *s = reinterpret_cast<const unsigned char *>(text.data());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        std::uint32_t cp = 0;
        std::size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f;
            len = 2;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f;
            len = 3;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07;
            len = 4;
        } else {
            ++i;
            continue;
        }
        if (i + len > n) {
            ++i;
            continue;
        }
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            if ((s[i + k] & 0xc0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (s[i + k] & 0x3f);
        }
        const bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                              (len == 4 && cp < 0x10000);
        if (!ok || overlong || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            ++i;
            continue;
        }
        fn(cp);
        i += len;
    }
}

std::int64_t nowMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::int64_t wallMs()
{
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return std::int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool writeAll(int fd, const char *data, std::size_t size)
{
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd p{fd, POLLOUT, 0};
                ::poll(&p, 1, 1000);
                continue;
            }
            return false;
        }
        data += n;
        size -= std::size_t(n);
    }
    return true;
}

bool writeAll(int fd, const std::string &data)
{
    return writeAll(fd, data.data(), data.size());
}

void setFlags(int fd, int add)
{
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | add);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

bool peerIsSameUser(int fd)
{
#if defined(__linux__)
    ucred cred{};
    socklen_t len = sizeof cred;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return false;
    return cred.uid == ::getuid();
#else
    uid_t uid = 0;
    gid_t gid = 0;
    return ::getpeereid(fd, &uid, &gid) == 0 && uid == ::getuid();
#endif
}

int g_signalPipe[2] = {-1, -1};

void onSignal(int sig)
{
    const int saved = errno;
    const unsigned char c = static_cast<unsigned char>(sig);
    [[maybe_unused]] const ssize_t n = ::write(g_signalPipe[1], &c, 1);
    errno = saved;
}

} // namespace

std::string usage()
{
    return "Usage: forkmesh term [--no-remote] [--] <cli> [args...]\n"
           "       forkmesh-term [--no-remote] [--] <cli> [args...]\n"
           "\n"
           "Runs <cli> (claude, codex, ...) in a terminal ForkMesh can type into, so your\n"
           "paired phone can send prompts as typed input. Everything you type still goes\n"
           "straight through. Ctrl-^ turns phone typing off and on for this session.\n"
           "\n"
           "  --no-remote   start with phone typing turned off\n"
           "  --help        show this help\n"
           "  --version     show the version\n";
}

bool parseArgs(const std::vector<std::string> &args, Options *options, std::string *error)
{
    std::size_t i = 0;
    if (!args.empty() && args.front() == "term")
        i = 1;
    for (; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "--") {
            ++i;
            break;
        }
        if (arg == "--no-remote") {
            options->remote = false;
        } else if (arg == "--remote") {
            options->remote = true;
        } else if (arg == "-h" || arg == "--help") {
            options->help = true;
        } else if (arg == "--version") {
            options->version = true;
        } else if (!arg.empty() && arg[0] == '-') {
            if (error)
                *error = "unknown option " + arg;
            return false;
        } else {
            break;
        }
    }
    options->argv.assign(args.begin() + std::ptrdiff_t(std::min(i, args.size())), args.end());
    if (options->argv.empty() && !options->help && !options->version) {
        if (error)
            *error = "name the program to run, for example: forkmesh term claude";
        return false;
    }
    return true;
}

std::string sanitizeText(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    bool lastCr = false;
    forEachCodePoint(text, [&](std::uint32_t cp) {
        if (cp == '\r') {
            out += '\n';
            lastCr = true;
            return;
        }
        if (cp == '\n') {
            if (!lastCr)
                out += '\n';
            lastCr = false;
            return;
        }
        lastCr = false;
        if (cp == '\t') {
            out += '\t';
            return;
        }
        if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f))
            return;
        appendUtf8(out, cp);
    });
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    return out;
}

std::string sanitizeLabel(const std::string &label)
{
    std::string out;
    std::size_t points = 0;
    forEachCodePoint(label, [&](std::uint32_t cp) {
        if (points >= 64)
            return;
        if (cp == '\n' || cp == '\r' || cp == '\t')
            cp = ' ';
        if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f))
            return;
        if (cp == ' ' && (out.empty() || out.back() == ' '))
            return;
        appendUtf8(out, cp);
        ++points;
    });
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out.empty() ? std::string("a paired device") : out;
}

std::string pasteBytes(const std::string &text, bool bracketed)
{
    if (bracketed)
        return "\x1b[200~" + text + "\x1b[201~";
    std::string flat = text;
    std::replace(flat.begin(), flat.end(), '\n', ' ');
    std::replace(flat.begin(), flat.end(), '\t', ' ');
    return flat;
}

std::string enterBytes()
{
    return "\r";
}

std::string interruptKeyName(const std::string &cli)
{
    return cli == "claude" || cli == "codex" ? "esc" : "ctrl-c";
}

std::string keyBytes(const std::string &key, const std::string &cli)
{
    const std::string name = key == "interrupt" ? interruptKeyName(cli) : key;
    if (name == "esc")
        return "\x1b";
    if (name == "ctrl-c")
        return "\x03";
    return {};
}

std::string cliName(const std::string &argv0)
{
    const std::size_t slash = argv0.find_last_of('/');
    std::string base = slash == std::string::npos ? argv0 : argv0.substr(slash + 1);
    if (base.rfind("claude", 0) == 0)
        return "claude";
    if (base.rfind("codex", 0) == 0)
        return "codex";
    return base;
}

std::string noticeLine(const std::string &what, const std::string &from)
{
    return "\x1b" "7\x1b[1B\r\x1b[2m\xe2\x8c\xa8 " + what + " " + sanitizeLabel(from) +
           "\x1b[0m\x1b" "8";
}

std::string jsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 2);
    for (unsigned char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    return out;
}

void PasteModeTracker::finish(char final)
{
    if ((final == 'h' || final == 'l') && !m_params.empty() && m_params[0] == '?') {
        std::stringstream list(m_params.substr(1));
        std::string item;
        while (std::getline(list, item, ';')) {
            if (item == "2004")
                m_enabled = final == 'h';
        }
    }
    m_params.clear();
    m_state = 0;
}

void PasteModeTracker::feed(const char *data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        switch (m_state) {
        case 0:
            if (c == '\x1b')
                m_state = 1;
            break;
        case 1:
            if (c == '[') {
                m_state = 2;
                m_params.clear();
            } else if (c == 'c') {
                m_enabled = false;
                m_state = 0;
            } else {
                m_state = c == '\x1b' ? 1 : 0;
            }
            break;
        default:
            if (c >= 0x40 && c <= 0x7e) {
                finish(c);
            } else if ((c >= 0x20 && c <= 0x3f) && m_params.size() < 32) {
                m_params += c;
            } else {
                m_params.clear();
                m_state = c == '\x1b' ? 1 : 0;
            }
        }
    }
}

bool RateLimiter::allow(std::int64_t now)
{
    while (!m_times.empty() && now - m_times.front() >= m_windowMs)
        m_times.pop_front();
    if (int(m_times.size()) >= m_max)
        return false;
    m_times.push_back(now);
    return true;
}

Parse parseRequest(const std::string &buffer, Request *request, std::size_t *consumed)
{
    const std::size_t eol = buffer.find('\n');
    if (eol == std::string::npos)
        return buffer.size() > kMaxHeaderBytes ? Parse::Bad : Parse::Incomplete;
    if (eol > kMaxHeaderBytes)
        return Parse::Bad;
    std::vector<std::string> tokens;
    std::stringstream header(buffer.substr(0, eol));
    std::string token;
    while (header >> token)
        tokens.push_back(token);
    if (tokens.size() != 4 || tokens[0] != "FMTERM" + std::to_string(kProtocolVersion) ||
        !validVerb(tokens[1]))
        return Parse::Bad;
    std::size_t fromLength = 0;
    std::size_t payloadLength = 0;
    if (!parseLength(tokens[2], &fromLength) || !parseLength(tokens[3], &payloadLength))
        return Parse::Bad;
    if (fromLength > kMaxFromBytes)
        return Parse::Bad;
    if (payloadLength > kMaxTextBytes)
        return Parse::TooLong;
    const std::size_t total = eol + 1 + fromLength + payloadLength;
    if (buffer.size() < total)
        return Parse::Incomplete;
    request->verb = tokens[1];
    request->from = buffer.substr(eol + 1, fromLength);
    request->payload = buffer.substr(eol + 1 + fromLength, payloadLength);
    *consumed = total;
    return Parse::Ok;
}

std::string requestFrame(const std::string &verb, const std::string &from, const std::string &payload)
{
    return "FMTERM" + std::to_string(kProtocolVersion) + " " + verb + " " +
           std::to_string(from.size()) + " " + std::to_string(payload.size()) + "\n" + from +
           payload;
}

std::string registrationJson(const Registration &r)
{
    std::string out = "{\"v\":" + std::to_string(kProtocolVersion);
    out += ",\"pid\":" + std::to_string(r.pid);
    out += ",\"start\":\"" + jsonEscape(r.start) + "\"";
    out += ",\"child\":" + std::to_string(r.child);
    out += ",\"cli\":\"" + jsonEscape(r.cli) + "\"";
    out += ",\"cwd\":\"" + jsonEscape(r.cwd) + "\"";
    out += ",\"socket\":\"" + jsonEscape(r.socket) + "\"";
    out += std::string(",\"remote\":") + (r.remote ? "true" : "false");
    out += ",\"startedMs\":" + std::to_string(r.startedMs);
    out += ",\"tty\":\"" + jsonEscape(r.tty) + "\"}\n";
    return out;
}

std::string runtimeDir()
{
    if (const char *forced = std::getenv("FORKMESH_TERM_DIR"); forced && *forced)
        return forced;
#if defined(__APPLE__)
    char buf[1024];
    const std::size_t n = ::confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
    if (n > 0 && n <= sizeof buf) {
        std::string dir(buf);
        if (!dir.empty() && dir.back() != '/')
            dir += '/';
        return dir + "forkmesh-term";
    }
#else
    if (const char *xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && xdg[0] == '/') {
        struct stat st {};
        if (::stat(xdg, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid())
            return std::string(xdg) + "/forkmesh-term";
    }
#endif
    return "/tmp/forkmesh-term-" + std::to_string(::getuid());
}

bool ensurePrivateDir(const std::string &path, std::string *error)
{
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        if (error)
            *error = path + ": " + std::strerror(errno);
        return false;
    }
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (error)
            *error = path + " is not a folder";
        return false;
    }
    if (st.st_uid != ::getuid()) {
        if (error)
            *error = path + " belongs to another user";
        return false;
    }
    if ((st.st_mode & 077) != 0 && ::chmod(path.c_str(), 0700) != 0) {
        if (error)
            *error = path + " is readable by other users";
        return false;
    }
    return true;
}

std::string procStart(long long pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    if (!in)
        return {};
    std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t close = stat.rfind(')');
    if (close == std::string::npos || close + 2 > stat.size())
        return {};
    std::stringstream fields(stat.substr(close + 2));
    std::string field;
    for (int i = 0; i <= 19; ++i) {
        if (!(fields >> field))
            return {};
    }
    return field;
}

namespace {

struct Client {
    int fd = -1;
    std::string buffer;
    std::int64_t deadline = 0;
};

struct Scheduled {
    std::int64_t at = 0;
    std::string bytes;
};

class Proxy {
public:
    explicit Proxy(const Options &options) : m_options(options) {}
    int exec();

private:
    bool setRaw();
    void restore();
    void syncSize();
    void suspend();
    void openControl();
    void writeRegistration();
    void removeControl();
    void notice(const std::string &what, const std::string &from);
    void onStdin();
    void onMaster();
    void flushMaster();
    void onAccept();
    void onClient(Client &client);
    std::string handle(const Request &request);
    void reap();

    Options m_options;
    std::string m_cli;
    termios m_saved{};
    bool m_haveSaved = false;
    bool m_raw = false;
    pid_t m_child = -1;
    int m_master = -1;
    int m_listener = -1;
    std::string m_dir;
    std::string m_socketPath;
    std::string m_registrationPath;
    Registration m_registration;
    std::vector<Client> m_clients;
    std::string m_toMaster;
    std::vector<Scheduled> m_scheduled;
    PasteModeTracker m_paste;
    RateLimiter m_typed{kMaxTypedPerWindow, kRateWindowMs};
    RateLimiter m_keys{kMaxKeysPerWindow, kRateWindowMs};
    std::int64_t m_lastLocalKeyMs = -kUserTypingQuietMs;
    bool m_remote = true;
    bool m_stdinOpen = true;
    bool m_masterOpen = true;
    bool m_childDone = false;
    std::int64_t m_childDoneAt = 0;
    int m_status = 0;
};

bool Proxy::setRaw()
{
    if (!m_haveSaved)
        return false;
    termios raw = m_saved;
    ::cfmakeraw(&raw);
    m_raw = ::tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) == 0;
    return m_raw;
}

void Proxy::restore()
{
    if (m_haveSaved && m_raw)
        ::tcsetattr(STDIN_FILENO, TCSADRAIN, &m_saved);
    m_raw = false;
}

void Proxy::syncSize()
{
    winsize size{};
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0 && m_master >= 0)
        ::ioctl(m_master, TIOCSWINSZ, &size);
}

void Proxy::suspend()
{
    restore();
    ::kill(::getpid(), SIGSTOP);
    setRaw();
    syncSize();
    ::kill(-m_child, SIGCONT);
}

void Proxy::notice(const std::string &what, const std::string &from)
{
    writeAll(STDOUT_FILENO, noticeLine(what, from));
}

void Proxy::writeRegistration()
{
    if (m_registrationPath.empty())
        return;
    m_registration.remote = m_remote;
    const std::string tmp = m_registrationPath + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return;
    const bool ok = writeAll(fd, registrationJson(m_registration));
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), m_registrationPath.c_str()) != 0)
        ::unlink(tmp.c_str());
}

void Proxy::openControl()
{
    std::string error;
    m_dir = runtimeDir();
    if (!ensurePrivateDir(m_dir, &error)) {
        std::fprintf(stderr, "forkmesh-term: phone typing is unavailable: %s\n", error.c_str());
        return;
    }
    const std::string pid = std::to_string(::getpid());
    const std::string socketPath = m_dir + "/" + pid + ".sock";
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof addr.sun_path) {
        std::fprintf(stderr, "forkmesh-term: phone typing is unavailable: socket path too long\n");
        return;
    }
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size());
    struct stat st {};
    if (::lstat(socketPath.c_str(), &st) == 0 && S_ISSOCK(st.st_mode))
        ::unlink(socketPath.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    const mode_t old = ::umask(0177);
    const int bound = ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
    ::umask(old);
    if (bound != 0 || ::chmod(socketPath.c_str(), 0600) != 0 || ::listen(fd, 4) != 0) {
        std::fprintf(stderr, "forkmesh-term: phone typing is unavailable: %s\n", std::strerror(errno));
        ::close(fd);
        return;
    }
    setFlags(fd, O_NONBLOCK);
    m_listener = fd;
    m_socketPath = socketPath;
    m_registrationPath = m_dir + "/" + pid + ".json";
    char cwd[4096];
    m_registration.pid = ::getpid();
    m_registration.start = procStart(::getpid());
    m_registration.child = m_child;
    m_registration.cli = m_cli;
    m_registration.cwd = ::getcwd(cwd, sizeof cwd) ? cwd : "";
    m_registration.socket = socketPath;
    m_registration.startedMs = wallMs();
    const char *tty = ::ttyname(STDIN_FILENO);
    m_registration.tty = tty ? tty : "";
    writeRegistration();
}

void Proxy::removeControl()
{
    for (Client &client : m_clients)
        ::close(client.fd);
    m_clients.clear();
    if (m_listener >= 0)
        ::close(m_listener);
    m_listener = -1;
    if (!m_socketPath.empty())
        ::unlink(m_socketPath.c_str());
    if (!m_registrationPath.empty())
        ::unlink(m_registrationPath.c_str());
}

void Proxy::onStdin()
{
    char buf[4096];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (n <= 0) {
        m_stdinOpen = false;
        if (n < 0 && m_child > 0)
            ::kill(m_child, SIGHUP);
        return;
    }
    std::string rest;
    rest.reserve(std::size_t(n));
    int toggles = 0;
    for (ssize_t i = 0; i < n; ++i) {
        if (static_cast<unsigned char>(buf[i]) == kToggleByte)
            ++toggles;
        else
            rest += buf[i];
    }
    if (!rest.empty()) {
        if (rest[0] != '\x1b')
            m_lastLocalKeyMs = nowMs();
        m_toMaster += rest;
    }
    if (toggles % 2 == 1) {
        m_remote = !m_remote;
        writeRegistration();
        notice(m_remote ? "phone typing on" : "phone typing off", "(Ctrl-^ toggles)");
    }
}

void Proxy::onMaster()
{
    char buf[65536];
    const ssize_t n = ::read(m_master, buf, sizeof buf);
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (n <= 0) {
        m_masterOpen = false;
        return;
    }
    m_paste.feed(buf, std::size_t(n));
    writeAll(STDOUT_FILENO, buf, std::size_t(n));
}

void Proxy::flushMaster()
{
    while (!m_toMaster.empty()) {
        const ssize_t n = ::write(m_master, m_toMaster.data(), m_toMaster.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                m_toMaster.clear();
            return;
        }
        m_toMaster.erase(0, std::size_t(n));
    }
}

void Proxy::onAccept()
{
    const int fd = ::accept(m_listener, nullptr, nullptr);
    if (fd < 0)
        return;
    if (!peerIsSameUser(fd) || m_clients.size() >= 4) {
        ::close(fd);
        return;
    }
    setFlags(fd, O_NONBLOCK);
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    m_clients.push_back(Client{fd, {}, nowMs() + 5000});
}

std::string Proxy::handle(const Request &request)
{
    const std::int64_t now = nowMs();
    if (request.verb == "hello")
        return "ok remote=" + std::string(m_remote ? "1" : "0") + " paste=" +
               (m_paste.enabled() ? "1" : "0") + " cli=" + m_cli + " child=" +
               std::to_string(m_child) + "\n";
    if (request.verb != "type" && request.verb != "key")
        return "err unknown_verb\n";
    if (!m_remote)
        return "err remote_off\n";
    if (m_childDone)
        return "err exited\n";
    if (request.verb == "type") {
        const std::string text = sanitizeText(request.payload);
        if (text.empty())
            return "err empty\n";
        if (now - m_lastLocalKeyMs < kUserTypingQuietMs)
            return "err user_typing\n";
        if (!m_typed.allow(now))
            return "err rate_limited\n";
        const bool bracketed = m_paste.enabled();
        notice("typed from", request.from);
        m_toMaster += pasteBytes(text, bracketed);
        m_scheduled.push_back(Scheduled{now + kEnterDelayMs, enterBytes()});
        return std::string("ok typed paste=") + (bracketed ? "1" : "0") + "\n";
    }
    const std::string name = request.payload == "interrupt" ? interruptKeyName(m_cli) : request.payload;
    const std::string bytes = keyBytes(name, m_cli);
    if (bytes.empty())
        return "err bad_key\n";
    if (!m_keys.allow(now))
        return "err rate_limited\n";
    notice(name == "esc" ? "Esc from" : "Ctrl-C from", request.from);
    m_toMaster += bytes;
    return "ok key=" + name + "\n";
}

void Proxy::onClient(Client &client)
{
    char buf[16384];
    bool ended = false;
    for (;;) {
        const ssize_t n = ::read(client.fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n <= 0) {
            ended = true;
            break;
        }
        client.buffer.append(buf, std::size_t(n));
        if (client.buffer.size() > kMaxHeaderBytes + kMaxFromBytes + kMaxTextBytes + 1)
            break;
    }
    Request request;
    std::size_t consumed = 0;
    std::string reply;
    switch (parseRequest(client.buffer, &request, &consumed)) {
    case Parse::Incomplete:
        if (ended)
            client.deadline = 0;
        return;
    case Parse::Bad:
        reply = "err bad_request\n";
        break;
    case Parse::TooLong:
        reply = "err too_long\n";
        break;
    case Parse::Ok:
        reply = handle(request);
        break;
    }
    ::send(client.fd, reply.data(), reply.size(),
#if defined(MSG_NOSIGNAL)
           MSG_NOSIGNAL
#else
           0
#endif
    );
    client.deadline = 0;
}

void Proxy::reap()
{
    for (;;) {
        int status = 0;
        const pid_t pid = ::waitpid(m_child, &status, WNOHANG | WUNTRACED);
        if (pid <= 0)
            return;
        if (WIFSTOPPED(status)) {
            suspend();
            continue;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            m_childDone = true;
            m_childDoneAt = nowMs();
            m_status = status;
            return;
        }
    }
}

int Proxy::exec()
{
    m_cli = cliName(m_options.argv.front());
    m_remote = m_options.remote;
    m_haveSaved = ::tcgetattr(STDIN_FILENO, &m_saved) == 0;
    winsize size{};
    const bool haveSize = ::ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0;
    if (::pipe(g_signalPipe) != 0)
        return 127;
    setFlags(g_signalPipe[0], O_NONBLOCK);
    setFlags(g_signalPipe[1], O_NONBLOCK);
    struct sigaction action {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (int sig : {SIGWINCH, SIGCHLD, SIGTERM, SIGHUP, SIGQUIT})
        ::sigaction(sig, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    m_child = ::forkpty(&m_master, nullptr, m_haveSaved ? &m_saved : nullptr,
                        haveSize ? &size : nullptr);
    if (m_child < 0) {
        std::fprintf(stderr, "forkmesh-term: cannot open a terminal: %s\n", std::strerror(errno));
        return 127;
    }
    if (m_child == 0) {
        for (int sig : {SIGWINCH, SIGCHLD, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE})
            ::signal(sig, SIG_DFL);
        ::setenv("FORKMESH_TERM", "1", 1);
        std::vector<char *> args;
        for (const std::string &arg : m_options.argv)
            args.push_back(const_cast<char *>(arg.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        std::fprintf(stderr, "forkmesh-term: cannot run %s: %s\r\n", args[0], std::strerror(errno));
        ::_exit(127);
    }
    setFlags(m_master, O_NONBLOCK);
    openControl();
    setRaw();

    for (;;) {
        std::vector<pollfd> fds;
        fds.push_back({g_signalPipe[0], POLLIN, 0});
        const std::size_t stdinAt = fds.size();
        if (m_stdinOpen)
            fds.push_back({STDIN_FILENO, POLLIN, 0});
        const std::size_t masterAt = fds.size();
        if (m_masterOpen)
            fds.push_back({m_master, short(POLLIN | (m_toMaster.empty() ? 0 : POLLOUT)), 0});
        const std::size_t listenerAt = fds.size();
        if (m_listener >= 0)
            fds.push_back({m_listener, POLLIN, 0});
        const std::size_t clientsAt = fds.size();
        for (const Client &client : m_clients)
            fds.push_back({client.fd, POLLIN, 0});

        std::int64_t now = nowMs();
        std::int64_t wake = -1;
        const auto soonest = [&](std::int64_t at) {
            const std::int64_t wait = std::max<std::int64_t>(0, at - now);
            wake = wake < 0 ? wait : std::min(wake, wait);
        };
        for (const Scheduled &item : m_scheduled)
            soonest(item.at);
        for (const Client &client : m_clients)
            soonest(client.deadline);
        if (m_childDone)
            soonest(m_childDoneAt + 200);
        const int ready = ::poll(fds.data(), nfds_t(fds.size()), int(wake));
        if (ready < 0 && errno != EINTR)
            break;
        now = nowMs();

        if (ready > 0 && (fds[0].revents & POLLIN)) {
            unsigned char sigs[64];
            ssize_t n = 0;
            while ((n = ::read(g_signalPipe[0], sigs, sizeof sigs)) > 0) {
                for (ssize_t i = 0; i < n; ++i) {
                    const int sig = sigs[i];
                    if (sig == SIGWINCH)
                        syncSize();
                    else if (sig == SIGCHLD)
                        reap();
                    else if (m_child > 0 && !m_childDone)
                        ::kill(m_child, sig);
                }
            }
        }
        if (ready > 0) {
            if (m_stdinOpen && (fds[stdinAt].revents & (POLLIN | POLLHUP | POLLERR)))
                onStdin();
            if (m_masterOpen && masterAt < fds.size() && fds[masterAt].fd == m_master) {
                if (fds[masterAt].revents & (POLLIN | POLLHUP | POLLERR))
                    onMaster();
                if (m_masterOpen && (fds[masterAt].revents & POLLOUT))
                    flushMaster();
            }
            if (m_listener >= 0 && listenerAt < fds.size() && fds[listenerAt].fd == m_listener &&
                (fds[listenerAt].revents & POLLIN))
                onAccept();
            for (std::size_t i = clientsAt; i < fds.size(); ++i) {
                if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                for (Client &client : m_clients) {
                    if (client.fd == fds[i].fd && client.deadline != 0)
                        onClient(client);
                }
            }
        }
        for (auto it = m_scheduled.begin(); it != m_scheduled.end();) {
            if (it->at <= now) {
                m_toMaster += it->bytes;
                it = m_scheduled.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = m_clients.begin(); it != m_clients.end();) {
            if (it->deadline == 0 || it->deadline <= now) {
                ::close(it->fd);
                it = m_clients.erase(it);
            } else {
                ++it;
            }
        }
        if (m_masterOpen && !m_toMaster.empty())
            flushMaster();
        if (!m_childDone)
            reap();
        if (m_childDone && (!m_masterOpen || now - m_childDoneAt >= 200))
            break;
    }
    while (m_masterOpen) {
        pollfd p{m_master, POLLIN, 0};
        if (::poll(&p, 1, 0) <= 0)
            break;
        onMaster();
    }
    removeControl();
    restore();
    if (m_master >= 0)
        ::close(m_master);
    if (!m_childDone) {
        ::kill(m_child, SIGHUP);
        ::waitpid(m_child, &m_status, 0);
    }
    if (WIFEXITED(m_status))
        return WEXITSTATUS(m_status);
    if (WIFSIGNALED(m_status))
        return 128 + WTERMSIG(m_status);
    return 1;
}

} // namespace

int run(const Options &options)
{
    if (!::isatty(STDIN_FILENO)) {
        std::vector<char *> args;
        for (const std::string &arg : options.argv)
            args.push_back(const_cast<char *>(arg.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        std::fprintf(stderr, "forkmesh-term: cannot run %s: %s\n", args[0], std::strerror(errno));
        return 127;
    }
    Proxy proxy(options);
    return proxy.exec();
}

int main(int argc, char **argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    Options options;
    std::string error;
    if (!parseArgs(args, &options, &error)) {
        std::fprintf(stderr, "forkmesh-term: %s\n\n%s", error.c_str(), usage().c_str());
        return 2;
    }
    if (options.help) {
        std::fputs(usage().c_str(), stdout);
        return 0;
    }
    if (options.version) {
        std::printf("forkmesh-term %s\n", FORKMESH_VERSION);
        return 0;
    }
    return run(options);
}

} // namespace forkmesh::term
