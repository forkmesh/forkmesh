#include "ForkMeshTerm.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#ifndef FORKMESH_TERM_BINARY
#error "FORKMESH_TERM_BINARY must name the built forkmesh-term"
#endif

using namespace forkmesh::term;

static int failures = 0;
static int checks = 0;

static void check(bool condition, const std::string &what)
{
    ++checks;
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void same(const std::string &got, const std::string &want, const std::string &what)
{
    ++checks;
    if (got == want)
        return;
    std::cerr << "FAIL: " << what << "\n  got:  " << got << "\n  want: " << want << "\n";
    ++failures;
}

static std::string hex(const std::string &bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

static std::string slurp(const std::string &path)
{
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool exists(const std::string &path)
{
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0;
}

static mode_t modeOf(const std::string &path)
{
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0 ? (st.st_mode & 0777) : 0;
}

static void testArgs()
{
    Options o;
    std::string error;
    check(parseArgs({"claude", "--model", "haiku"}, &o, &error), "plain args parse");
    check(o.remote && o.argv == std::vector<std::string>{"claude", "--model", "haiku"},
          "cli arguments kept, flags after the cli belong to it");
    Options t;
    check(parseArgs({"term", "--no-remote", "--", "codex", "-m", "x"}, &t, &error),
          "forkmesh term form parses");
    check(!t.remote && t.argv == std::vector<std::string>{"codex", "-m", "x"}, "term form argv");
    Options e;
    check(!parseArgs({}, &e, &error) && !error.empty(), "a cli is required");
    Options u;
    check(!parseArgs({"--bogus", "claude"}, &u, &error), "unknown option refused");
    Options h;
    check(parseArgs({"--help"}, &h, &error) && h.help, "help needs no cli");
}

static void testText()
{
    same(sanitizeText("hi\r\nthere\rnow\n\n"), "hi\nthere\nnow", "line endings folded and trailing newlines dropped");
    same(sanitizeText("a\x1b[201~b\x07" "c\xc2\x9b" "d\x7f" "e\tf"), "a[201~bcde\tf",
         "escape, bell, C1 and DEL dropped so a paste cannot be closed early");
    same(sanitizeText("caf\xc3\xa9 \xe2\x8c\xa8 \xf0\x9f\x98\x80"), "caf\xc3\xa9 \xe2\x8c\xa8 \xf0\x9f\x98\x80",
         "UTF-8 kept");
    same(sanitizeText("x\xff\xc0\xafy"), "xy", "invalid and overlong UTF-8 dropped");
    same(sanitizeLabel("Jett's\niPhone\x1b[31m"), "Jett's iPhone[31m", "label flattened");
    same(sanitizeLabel("\x01\x02"), "a paired device", "empty label replaced");
    check(sanitizeLabel(std::string(200, 'x')).size() == 64, "label capped at 64");
    same(pasteBytes("a\nb", true), "\x1b[200~a\nb\x1b[201~", "bracketed paste framing");
    same(pasteBytes("a\nb\tc", false), "a b c", "without bracketed paste newlines become spaces");
    same(enterBytes(), "\r", "enter is CR");
    same(keyBytes("esc", "codex"), "\x1b", "esc key");
    same(keyBytes("ctrl-c", "claude"), "\x03", "ctrl-c key");
    same(keyBytes("interrupt", "claude"), "\x1b", "claude interrupts with esc");
    same(keyBytes("interrupt", "codex"), "\x1b", "codex interrupts with esc");
    same(keyBytes("interrupt", "aider"), "\x03", "other clis interrupt with ctrl-c");
    same(keyBytes("rm", "claude"), "", "unknown key refused");
    same(cliName("/home/u/.local/bin/claude"), "claude", "cli from path");
    same(cliName("codex-x86_64-unknown-linux-musl"), "codex", "codex variants");
    same(cliName("bash"), "bash", "other cli");
    const std::string notice = noticeLine("typed from", "Jett's iPhone");
    check(notice.find("typed from Jett's iPhone") != std::string::npos, "notice names the device");
    check(notice.rfind("\x1b" "7", 0) == 0 && notice.size() > 2 &&
              notice.compare(notice.size() - 2, 2, "\x1b" "8") == 0,
          "notice saves and restores the cursor");
    check(notice.find('\n') == std::string::npos, "notice never scrolls the screen");
    same(jsonEscape("a\"b\\c\n\x01"), "a\\\"b\\\\c\\n\\u0001", "json escape");
}

static void testTracker()
{
    PasteModeTracker t;
    check(!t.enabled(), "off at start");
    t.feed("\x1b[?20", 5);
    t.feed("04h", 3);
    check(t.enabled(), "enable split across reads");
    t.feed("\x1b[?2004l", 8);
    check(!t.enabled(), "disable");
    const std::string combined = "text\x1b[?1049;2004hmore";
    t.feed(combined.data(), combined.size());
    check(t.enabled(), "combined private modes");
    t.feed("\x1b" "c", 2);
    check(!t.enabled(), "full reset disables");
    const std::string other = "\x1b[2004h\x1b[?20045h";
    t.feed(other.data(), other.size());
    check(!t.enabled(), "non-private and other modes ignored");
}

static void testLimiter()
{
    RateLimiter r(3, 1000);
    check(r.allow(0) && r.allow(10) && r.allow(20), "three allowed");
    check(!r.allow(30), "fourth refused");
    check(!r.allow(999), "still inside the window");
    check(r.allow(1000), "window slides");
}

static void testFraming()
{
    const std::string frame = requestFrame("type", "Phone", "hello");
    same(frame, "FMTERM1 type 5 5\nPhonehello", "frame layout");
    Request r;
    std::size_t used = 0;
    check(parseRequest(frame.substr(0, 10), &r, &used) == Parse::Incomplete, "partial header");
    check(parseRequest(frame.substr(0, frame.size() - 1), &r, &used) == Parse::Incomplete,
          "partial payload");
    check(parseRequest(frame + "FMTERM1 hello 0 0\n", &r, &used) == Parse::Ok && used == frame.size() &&
              r.verb == "type" && r.from == "Phone" && r.payload == "hello",
          "complete frame, extra bytes left");
    check(parseRequest("GET / HTTP/1.1\r\n", &r, &used) == Parse::Bad, "foreign protocol");
    check(parseRequest("FMTERM2 type 0 1\nx", &r, &used) == Parse::Bad, "other version");
    check(parseRequest("FMTERM1 TYPE 0 1\nx", &r, &used) == Parse::Bad, "verb charset");
    check(parseRequest("FMTERM1 type 0 -1\n", &r, &used) == Parse::Bad, "negative length");
    check(parseRequest("FMTERM1 type 999 1\n", &r, &used) == Parse::Bad, "long label");
    check(parseRequest("FMTERM1 type 0 70000\n", &r, &used) == Parse::TooLong, "long payload");
    check(parseRequest(std::string(200, 'A'), &r, &used) == Parse::Bad, "header without newline");
    Registration reg;
    reg.pid = 12;
    reg.child = 13;
    reg.start = "99";
    reg.cli = "claude";
    reg.cwd = "/tmp/a \"b\"";
    reg.socket = "/run/user/1/forkmesh-term/12.sock";
    reg.remote = false;
    reg.startedMs = 5;
    reg.tty = "/dev/pts/1";
    same(registrationJson(reg),
         "{\"v\":1,\"pid\":12,\"start\":\"99\",\"child\":13,\"cli\":\"claude\",\"cwd\":\"/tmp/a "
         "\\\"b\\\"\",\"socket\":\"/run/user/1/forkmesh-term/12.sock\",\"remote\":false,"
         "\"startedMs\":5,\"tty\":\"/dev/pts/1\"}\n",
         "registration json");
}

static void testPrivateDir(const std::string &root)
{
    const std::string dir = root + "/private";
    std::string error;
    check(ensurePrivateDir(dir, &error) && modeOf(dir) == 0700, "created 0700");
    ::chmod(dir.c_str(), 0755);
    check(ensurePrivateDir(dir, &error) && modeOf(dir) == 0700, "tightened to 0700");
    const std::string link = root + "/link";
    [[maybe_unused]] const int linked = ::symlink(dir.c_str(), link.c_str());
    check(!ensurePrivateDir(link, &error), "symlink refused");
}

struct Run {
    pid_t pid = -1;
    int master = -1;
    std::string out;
    int status = -1;
};

static Run launch(const std::string &dir, const std::vector<std::string> &args, int rows = 24,
                  int cols = 80)
{
    Run run;
    winsize size{};
    size.ws_row = static_cast<unsigned short>(rows);
    size.ws_col = static_cast<unsigned short>(cols);
    run.pid = ::forkpty(&run.master, nullptr, nullptr, &size);
    if (run.pid == 0) {
        ::setenv("FORKMESH_TERM_DIR", dir.c_str(), 1);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(FORKMESH_TERM_BINARY));
        for (const std::string &arg : args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        ::execv(FORKMESH_TERM_BINARY, argv.data());
        ::_exit(126);
    }
    ::fcntl(run.master, F_SETFL, ::fcntl(run.master, F_GETFL) | O_NONBLOCK);
    return run;
}

static void pump(Run &run, int ms)
{
    pollfd p{run.master, POLLIN, 0};
    if (::poll(&p, 1, ms) <= 0)
        return;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(run.master, buf, sizeof buf);
        if (n <= 0)
            break;
        run.out.append(buf, std::size_t(n));
    }
}

static bool readUntil(Run &run, const std::string &needle, int timeoutMs = 5000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (run.out.find(needle) == std::string::npos) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        pump(run, 50);
    }
    return true;
}

static int finish(Run &run, int timeoutMs = 5000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        pump(run, 20);
        int status = 0;
        const pid_t done = ::waitpid(run.pid, &status, WNOHANG);
        if (done == run.pid) {
            pump(run, 20);
            ::close(run.master);
            run.status = status;
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        if (std::chrono::steady_clock::now() > end) {
            ::kill(run.pid, SIGKILL);
            ::waitpid(run.pid, &status, 0);
            ::close(run.master);
            return -1;
        }
    }
}

static void type(Run &run, const std::string &bytes)
{
    [[maybe_unused]] const ssize_t n = ::write(run.master, bytes.data(), bytes.size());
}

static std::string ask(const std::string &socket, const std::string &frame)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socket.c_str(), std::min(socket.size(), sizeof addr.sun_path - 1));
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return "connect failed";
    }
    std::size_t sent = 0;
    while (sent < frame.size()) {
        const ssize_t n = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
            break;
        sent += std::size_t(n);
    }
    ::shutdown(fd, SHUT_WR);
    std::string reply;
    char buf[256];
    pollfd p{fd, POLLIN, 0};
    while (::poll(&p, 1, 3000) > 0) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        reply.append(buf, std::size_t(n));
    }
    ::close(fd);
    return reply;
}

static bool waitFor(Run &run, const std::string &path, int timeoutMs = 5000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!exists(path)) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        pump(run, 20);
    }
    return true;
}

static bool waitPaste(Run &run, const std::string &socket)
{
    for (int i = 0; i < 100; ++i) {
        if (ask(socket, requestFrame("hello", "", "")).find("paste=1") != std::string::npos)
            return true;
        pump(run, 30);
    }
    return false;
}

static const char *kReader =
    "stty raw -echo; printf 'READY'; printf 'BYTES:'; dd bs=1 count=%d 2>/dev/null | od -An -tx1 | "
    "tr -d ' \\n'; printf ':END'";

static std::string reader(int count, bool bracketed)
{
    char buf[512];
    std::snprintf(buf, sizeof buf, kReader, count);
    return std::string(bracketed ? "printf '\\033[?2004h'; " : "") + buf;
}

static std::string bytesSeen(const std::string &out)
{
    const std::size_t a = out.find("BYTES:");
    const std::size_t b = out.find(":END");
    if (a == std::string::npos || b == std::string::npos || b < a)
        return "(none)";
    std::string seen = out.substr(a + 6, b - a - 6);
    for (std::size_t open = seen.find("\x1b" "7"); open != std::string::npos;
         open = seen.find("\x1b" "7")) {
        const std::size_t close = seen.find("\x1b" "8", open);
        if (close == std::string::npos)
            break;
        seen.erase(open, close + 2 - open);
    }
    return seen;
}

static void testPassthrough(const std::string &dir)
{
    Run run = launch(dir, {"sh", "-c", "stty size; read line; echo \"got:$line\"; exit 3"});
    check(readUntil(run, "24 80"), "child sees the terminal size");
    type(run, "hello\r");
    check(readUntil(run, "got:hello"), "typed keys pass through");
    same(std::to_string(finish(run)), "3", "exit code propagates");
    same(slurp(dir + "/" + std::to_string(run.pid) + ".json"), "", "registration removed on exit");
    check(!exists(dir + "/" + std::to_string(run.pid) + ".sock"), "socket removed on exit");
}

static void testResize(const std::string &dir)
{
    Run run = launch(dir, {"sh", "-c", "read a; stty size; exit 0"});
    check(waitFor(run, dir + "/" + std::to_string(run.pid) + ".json"), "registered");
    winsize size{};
    size.ws_row = 30;
    size.ws_col = 100;
    ::ioctl(run.master, TIOCSWINSZ, &size);
    pump(run, 300);
    type(run, "x\r");
    check(readUntil(run, "30 100"), "resize reaches the child");
    same(std::to_string(finish(run)), "0", "clean exit");
}

static void testSignalExit(const std::string &dir)
{
    Run run = launch(dir, {"sh", "-c", "kill -TERM $$"});
    same(std::to_string(finish(run)), "143", "death by signal becomes 128+signal");
    Run missing = launch(dir, {"forkmesh-term-no-such-program"});
    same(std::to_string(finish(missing)), "127", "missing program is 127");
}

static void testRegistration(const std::string &dir)
{
    Run run = launch(dir, {"sh", "-c", "sleep 10"});
    const std::string base = dir + "/" + std::to_string(run.pid);
    check(waitFor(run, base + ".json"), "registration written");
    const std::string json = slurp(base + ".json");
    check(json.find("\"pid\":" + std::to_string(run.pid)) != std::string::npos, "own pid");
    check(json.find("\"cli\":\"sh\"") != std::string::npos, "cli named");
    check(json.find("\"remote\":true") != std::string::npos, "remote on by default");
    check(json.find("\"socket\":\"" + base + ".sock\"") != std::string::npos, "socket path");
    same(std::to_string(modeOf(base + ".sock")), std::to_string(0600), "socket is 0600");
    same(std::to_string(modeOf(base + ".json")), std::to_string(0600), "registration is 0600");
    same(std::to_string(modeOf(dir)), std::to_string(0700), "folder is 0700");
    const std::string hello = ask(base + ".sock", requestFrame("hello", "", ""));
    check(hello.rfind("ok remote=1 paste=0 cli=sh child=", 0) == 0, "hello answers: " + hello);
    same(ask(base + ".sock", "GARBAGE\n"), "err bad_request\n", "garbage refused");
    same(ask(base + ".sock", "FMTERM1 type 0 70000\n"), "err too_long\n", "oversize refused");
    same(ask(base + ".sock", requestFrame("paste", "p", "x")), "err unknown_verb\n", "unknown verb");
    same(ask(base + ".sock", requestFrame("type", "p", "\x1b\x07")), "err empty\n",
         "nothing left after sanitising");
    same(ask(base + ".sock", requestFrame("key", "p", "f1")), "err bad_key\n", "unknown key");
    ::kill(run.pid, SIGTERM);
    check(finish(run) >= 0, "SIGTERM ends the session");
    check(!exists(base + ".json"), "registration removed after SIGTERM");
}

static void testInjection(const std::string &dir)
{
    {
        Run run = launch(dir, {"sh", "-c", reader(21, true)});
        const std::string sock = dir + "/" + std::to_string(run.pid) + ".sock";
        check(waitFor(run, sock) && readUntil(run, "READY"), "reader ready");
        check(waitPaste(run, sock), "bracketed paste noticed");
        same(ask(sock, requestFrame("type", "Test Phone", "hi\r\nthere")), "ok typed paste=1\n",
             "typed");
        check(readUntil(run, ":END"), "reader finished");
        same(bytesSeen(run.out), hex("\x1b[200~hi\nthere\x1b[201~\r"),
             "multi-line prompt arrives as one bracketed paste, then Enter");
        check(run.out.find("typed from Test Phone") != std::string::npos, "notice shown");
        check(run.out.find("typed from Test Phone") < run.out.find(":END"), "notice before the text");
        finish(run);
    }
    {
        const std::string sneaky = "a\x1b[201~b\x07" "c";
        const std::string expect = "\x1b[200~a[201~bc\x1b[201~\r";
        Run run = launch(dir, {"sh", "-c", reader(int(expect.size()), true)});
        const std::string sock = dir + "/" + std::to_string(run.pid) + ".sock";
        check(waitFor(run, sock) && readUntil(run, "READY") && waitPaste(run, sock), "reader ready");
        same(ask(sock, requestFrame("type", "Test Phone", sneaky)), "ok typed paste=1\n", "typed");
        check(readUntil(run, ":END"), "reader finished");
        same(bytesSeen(run.out), hex(expect), "control bytes cannot leave the paste");
        finish(run);
    }
    {
        Run run = launch(dir, {"sh", "-c", reader(9, false)});
        const std::string sock = dir + "/" + std::to_string(run.pid) + ".sock";
        check(waitFor(run, sock) && readUntil(run, "READY"), "reader ready");
        same(ask(sock, requestFrame("type", "Test Phone", "hi\nthere")), "ok typed paste=0\n", "typed");
        check(readUntil(run, ":END"), "reader finished");
        same(bytesSeen(run.out), hex("hi there\r"), "no bracketed paste: one line, then Enter");
        finish(run);
    }
    {
        Run run = launch(dir, {"sh", "-c", reader(1, false)});
        const std::string sock = dir + "/" + std::to_string(run.pid) + ".sock";
        check(waitFor(run, sock) && readUntil(run, "READY"), "reader ready");
        same(ask(sock, requestFrame("key", "Test Phone", "esc")), "ok key=esc\n", "esc sent");
        check(readUntil(run, ":END"), "reader finished");
        same(bytesSeen(run.out), "1b", "esc arrives as a lone ESC byte");
        check(run.out.find("Esc from Test Phone") != std::string::npos, "stop notice shown");
        finish(run);
    }
    {
        Run run = launch(dir, {"sh", "-c", reader(1, false)});
        const std::string sock = dir + "/" + std::to_string(run.pid) + ".sock";
        check(waitFor(run, sock) && readUntil(run, "READY"), "reader ready");
        same(ask(sock, requestFrame("key", "Test Phone", "interrupt")), "ok key=ctrl-c\n",
             "interrupt picks ctrl-c for a non-claude cli");
        check(readUntil(run, ":END"), "reader finished");
        same(bytesSeen(run.out), "03", "ctrl-c arrives as ETX");
        finish(run);
    }
}

static void testGuards(const std::string &dir)
{
    {
        Run run = launch(dir, {"--no-remote", "sh", "-c", "sleep 10"});
        const std::string base = dir + "/" + std::to_string(run.pid);
        check(waitFor(run, base + ".json"), "registered");
        check(slurp(base + ".json").find("\"remote\":false") != std::string::npos,
              "--no-remote registers remote off");
        same(ask(base + ".sock", requestFrame("type", "p", "hi")), "err remote_off\n",
             "typing refused while off");
        same(ask(base + ".sock", requestFrame("key", "p", "esc")), "err remote_off\n",
             "keys refused while off");
        type(run, "\x1e");
        pump(run, 300);
        check(ask(base + ".sock", requestFrame("hello", "", "")).rfind("ok remote=1", 0) == 0,
              "Ctrl-^ turns phone typing on");
        check(slurp(base + ".json").find("\"remote\":true") != std::string::npos,
              "registration follows the toggle");
        check(readUntil(run, "phone typing on"), "toggle notice shown");
        type(run, "\x1e");
        pump(run, 300);
        check(ask(base + ".sock", requestFrame("hello", "", "")).rfind("ok remote=0", 0) == 0,
              "Ctrl-^ turns it off again");
        ::kill(run.pid, SIGTERM);
        finish(run);
    }
    {
        Run run = launch(dir, {"sh", "-c", "sleep 10"});
        const std::string sock = dir + "/" + std::to_string(run.pid) + ".sock";
        check(waitFor(run, sock), "registered");
        type(run, "x");
        pump(run, 50);
        same(ask(sock, requestFrame("type", "p", "hi")), "err user_typing\n",
             "refused while someone types at the keyboard");
        std::this_thread::sleep_for(std::chrono::milliseconds(kUserTypingQuietMs + 200));
        same(ask(sock, requestFrame("type", "p", "hi")), "ok typed paste=0\n", "accepted once quiet");
        int accepted = 1;
        std::string last;
        for (int i = 0; i < kMaxTypedPerWindow + 2; ++i) {
            last = ask(sock, requestFrame("type", "p", "hi"));
            if (last.rfind("ok", 0) == 0)
                ++accepted;
        }
        same(std::to_string(accepted), std::to_string(kMaxTypedPerWindow), "typing capped per minute");
        same(last, "err rate_limited\n", "over the cap");
        ::kill(run.pid, SIGTERM);
        finish(run);
    }
}

static void testNotATerminal(const std::string &dir)
{
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int null = ::open("/dev/null", O_RDONLY);
        ::dup2(null, STDIN_FILENO);
        ::setenv("FORKMESH_TERM_DIR", dir.c_str(), 1);
        ::execl(FORKMESH_TERM_BINARY, FORKMESH_TERM_BINARY, "sh", "-c", "exit 4", nullptr);
        ::_exit(126);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    same(std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1), "4",
         "without a terminal the cli runs directly");
    check(!exists(dir + "/" + std::to_string(pid) + ".json"), "and nothing is registered");
}

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    char tmpl[] = "/tmp/forkmesh-term-test-XXXXXX";
    const char *root = ::mkdtemp(tmpl);
    if (!root) {
        std::cerr << "mkdtemp failed\n";
        return 1;
    }
    const std::string dir = std::string(root) + "/rt";
    std::string error;
    check(ensurePrivateDir(dir, &error), "runtime folder");
    testArgs();
    testText();
    testTracker();
    testLimiter();
    testFraming();
    testPrivateDir(root);
    testPassthrough(dir);
    testResize(dir);
    testSignalExit(dir);
    testRegistration(dir);
    testInjection(dir);
    testGuards(dir);
    testNotATerminal(dir);
    ::unlink((std::string(root) + "/link").c_str());
    ::rmdir((std::string(root) + "/private").c_str());
    ::rmdir(dir.c_str());
    ::rmdir(root);
    std::cout << checks << " checks, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
