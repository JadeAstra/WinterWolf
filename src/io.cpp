// Streams, child processes and sockets: the built-in __io, which src/std/io.ww is
// written on.
//
// Everything here that can wait (a read, a write, an accept, waiting for a process to end)
// goes through VM::blocking. Called on a thread, it blocks that thread with the VM's lock
// released, so the other threads go on. Called in a task on the main thread, the task is
// set aside while a worker thread waits, and the event loop goes on. There is one set of
// functions for both.
//
// What the operating system does is below in two parts, one for Windows and one for the
// systems with POSIX's calls, Linux and macOS (G56): a Stream, reading and writing it, and the
// operations on files, processes and sockets. The functions programs call are written once,
// on those.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>  // AF_UNIX: Windows 10 1803 on
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET  // a UDP socket's ICMP "port unreachable" reports, in mstcpip.h of newer SDKs
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern char** environ;
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "builtins.h"
#include "charset.h"
#include "sandbox.h"
#include "vm.h"

namespace ww {

namespace {

WwThrow ioError(VM& vm, const std::string& message) { return vm.errorOf("IOException", message); }

// What a blocking piece of work leaves for the function that turns it into a value.
struct Stream;
struct Outcome {
    std::string data;
    std::string error;
    bool end = false;
    int64_t number = 0;
    std::shared_ptr<Stream> made;
    bool timedOut = false;  // a read ran past the socket's readTimeout
    bool unknownHost = false;  // a name the system could not find: an UnknownHostException
    std::vector<std::string> texts;  // the addresses a name was found at
    std::string address;  // where a datagram came from, and its port in `number`
};

// A read that ran past a socket's readTimeout is a SocketTimeoutException, an IOException as in
// Java (D120), told by the mark the wait left, not by the message's text (8.8f).
WwThrow ioError(VM& vm, const Outcome& out) { return vm.errorOf(out.timedOut ? "SocketTimeoutException" : out.unknownHost ? "UnknownHostException" : "IOException", out.error); }

// The buffers a read into the program's bytes goes through (RandomAccessFile.readAt into a
// buffer), kept for the next read: a file read a megabyte at a time would otherwise take new
// memory from the system for each, and pay for touching it the first time, which was most of
// the read's time. A few are kept, none over 16 MB.
class ScratchBuffers {
public:
    std::vector<uint8_t> take(size_t size) {
        {
            std::lock_guard<std::mutex> hold(mutex_);
            for (size_t i = 0; i < kept_.size(); i++)
                if (kept_[i].size() >= size) {
                    std::vector<uint8_t> found = std::move(kept_[i]);
                    kept_.erase(kept_.begin() + static_cast<std::ptrdiff_t>(i));
                    return found;
                }
        }
        return std::vector<uint8_t>(size);
    }
    void give(std::vector<uint8_t>&& buffer) {
        if (buffer.size() == 0 || buffer.size() > (size_t{16} << 20)) return;
        std::lock_guard<std::mutex> hold(mutex_);
        if (kept_.size() < 4) kept_.push_back(std::move(buffer));
    }
private:
    std::mutex mutex_;
    std::vector<std::vector<uint8_t>> kept_;
};
ScratchBuffers& scratchBuffers() {
    static ScratchBuffers* buffers = new ScratchBuffers();  // never destroyed: a read may end as the program does
    return *buffers;
}
// A read's buffer and what came of it, the buffer given back when the read is done with.
struct ScratchRead : Outcome {
    std::vector<uint8_t> buffer;
    int64_t got = 0;
    ~ScratchRead() { scratchBuffers().give(std::move(buffer)); }
};

// ---- A sandbox's programs and pipes (M6.6.2 step 4) ----

// A pipe between two ends in a sandbox: what is written is kept until it is read, however much.
struct PipeBuffer {
    std::mutex mutex;
    std::condition_variable changed;
    std::string data;
    bool writerClosed = false;
    bool readerClosed = false;
};

class PipeEnd final : public Channel {
public:
    PipeEnd(std::shared_ptr<PipeBuffer> buffer, bool reading) : buffer_(std::move(buffer)), reading_(reading) {}
    int read(char* out, size_t max, std::string& error, bool&) override {
        if (!reading_) {
            error = "the stream is not open for reading";
            return -1;
        }
        std::unique_lock<std::mutex> hold(buffer_->mutex);
        buffer_->changed.wait(hold, [&] { return !buffer_->data.empty() || buffer_->writerClosed || buffer_->readerClosed; });
        if (buffer_->readerClosed) {
            error = "the stream is closed";
            return -1;
        }
        if (buffer_->data.empty()) return 0;  // the writer has closed its end
        const size_t count = std::min(max, buffer_->data.size());
        std::copy_n(buffer_->data.data(), count, out);
        buffer_->data.erase(0, count);
        return static_cast<int>(count);
    }
    bool write(const char* data, size_t size, std::string& error) override {
        std::lock_guard<std::mutex> hold(buffer_->mutex);
        if (reading_ || buffer_->writerClosed || buffer_->readerClosed) {
            error = reading_ ? "the stream is not open for writing" : "the pipe is closed";
            return false;
        }
        buffer_->data.append(data, size);
        buffer_->changed.notify_all();
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> hold(buffer_->mutex);
        (reading_ ? buffer_->readerClosed : buffer_->writerClosed) = true;
        buffer_->changed.notify_all();
    }

private:
    std::shared_ptr<PipeBuffer> buffer_;
    bool reading_;
};

// A connection in a sandbox (step 5): what each end writes, kept for the other until read.
// Either may reset it; an end may wait for ever, so that a read of the other's with a time limit
// is up at once, by the sandbox's clock.
struct SocketLink {
    std::mutex mutex;
    std::condition_variable changed;
    std::string forEnd[2];         // what is waiting to be read by end 0 and by end 1
    bool doneWriting[2] = {false, false};
    bool gone[2] = {false, false};  // closed entirely
    bool hanging[2] = {false, false};
    bool reset = false;
    std::shared_ptr<Host> host;
};

class SocketEnd final : public Channel {
public:
    SocketEnd(std::shared_ptr<SocketLink> link, int side, std::string remoteHost, int remotePort, int localPort)
        : remoteHost(std::move(remoteHost)), remotePort(remotePort), localPort(localPort), link_(std::move(link)), side_(side) {}
    int read(char* out, size_t max, std::string& error, bool& timedOut) override {
        SocketLink& l = *link_;
        const int other = 1 - side_;
        std::unique_lock<std::mutex> hold(l.mutex);
        l.changed.wait(hold, [&] {
            return !l.forEnd[side_].empty() || l.doneWriting[other] || l.gone[other] || l.reset || l.gone[side_] || (l.hanging[other] && timeout_ > 0);
        });
        if (l.gone[side_]) {
            error = "the socket is closed";
            return -1;
        }
        // What was sent before a reset is read first, so that a test sees the same thing however
        // the threads ran.
        if (!l.forEnd[side_].empty()) {
            const size_t count = std::min(max, l.forEnd[side_].size());
            std::copy_n(l.forEnd[side_].data(), count, out);
            l.forEnd[side_].erase(0, count);
            return static_cast<int>(count);
        }
        if (l.reset) {
            error = "the connection was reset";
            return -1;
        }
        if (l.doneWriting[other] || l.gone[other]) return 0;
        // The other side waits for ever: the time limit is up, the clock moved on by it.
        if (l.host != nullptr && l.host->isSandbox()) static_cast<SandboxHost&>(*l.host).advance(timeout_ * 1000000);
        timedOut = true;
        error = "the read timed out";
        return -1;
    }
    bool write(const char* data, size_t size, std::string& error) override {
        SocketLink& l = *link_;
        std::lock_guard<std::mutex> hold(l.mutex);
        if (l.reset || l.gone[1 - side_]) {
            error = l.reset ? "the connection was reset" : "the other side has closed the connection";
            return false;
        }
        if (l.doneWriting[side_] || l.gone[side_]) {
            error = "the socket is closed";
            return false;
        }
        l.forEnd[1 - side_].append(data, size);
        l.changed.notify_all();
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> hold(link_->mutex);
        link_->gone[side_] = true;
        link_->doneWriting[side_] = true;
        link_->changed.notify_all();
    }
    void shutdownOutput() override {
        std::lock_guard<std::mutex> hold(link_->mutex);
        link_->doneWriting[side_] = true;
        link_->changed.notify_all();
    }
    void setReadTimeout(int64_t ms) override { timeout_ = ms; }
    void resetLink() {
        std::lock_guard<std::mutex> hold(link_->mutex);
        link_->reset = true;
        link_->changed.notify_all();
    }
    // This end waits for ever: until the connection is closed or reset.
    void hang() {
        std::unique_lock<std::mutex> hold(link_->mutex);
        link_->hanging[side_] = true;
        link_->changed.notify_all();
        link_->changed.wait(hold, [&] { return link_->reset || link_->gone[0] || link_->gone[1]; });
    }

    const std::string remoteHost;
    const int remotePort;
    const int localPort;

private:
    std::shared_ptr<SocketLink> link_;
    int side_;
    int64_t timeout_ = 0;
};

// A port a program listens on in a sandbox: the connections made to it, waiting for accept.
struct SandboxListener {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Stream>> waiting;
    bool closed = false;
    int port = 0;
};

// A program of a sandbox's: a WinterWolf thread running what the test declared, its end told here.
struct SimulatedProcess {
    std::mutex mutex;
    std::condition_variable changed;
    bool ended = false;
    int code = 0;
    bool hanging = false;  // it waits for ever (SandboxProcess.waitForever): a time limit on it is up at once
    int64_t pid = 0;
    std::shared_ptr<Host> host;                          // whose clock such a time limit moves on
    std::vector<std::shared_ptr<Channel>> programEnds;  // its own ends of the pipes, closed when it is killed
};

// Waits for it to end, `limit` ms at most (-1 for none): whether it did. A limit on one that waits
// for ever moves its sandbox's clock on by the limit, and is up at once.
bool waitSimulated(SimulatedProcess& p, int64_t limit, int& code) {
    std::unique_lock<std::mutex> hold(p.mutex);
    for (;;) {
        if (p.ended) {
            code = p.code;
            return true;
        }
        if (limit == 0) return false;
        if (p.hanging && limit > 0) {
            if (p.host != nullptr && p.host->isSandbox()) static_cast<SandboxHost&>(*p.host).advance(limit * 1000000);
            return false;
        }
        p.changed.wait(hold);
    }
}

// Its end, with `code`; killed, its ends of the pipes are closed, so that what reads them ends.
void endSimulated(SimulatedProcess& p, int code, bool killed) {
    std::vector<std::shared_ptr<Channel>> ends;
    {
        std::lock_guard<std::mutex> hold(p.mutex);
        if (p.ended) return;
        p.ended = true;
        p.code = code;
        if (killed) ends = p.programEnds;
        p.changed.notify_all();
    }
    for (const auto& end : ends) end->close();
}

#ifdef _WIN32

// ---- Windows: streams ----

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string narrow(const wchar_t* wide, int length = -1) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, length, utf8.data(), size, nullptr, nullptr);
    if (length < 0 && !utf8.empty() && utf8.back() == '\0') utf8.pop_back();
    return utf8;
}

// What Windows has to say about an error code.
std::string systemMessage(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                                        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string message = length > 0 ? narrow(text, static_cast<int>(length)) : "error " + std::to_string(code);
    if (text != nullptr) LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.' || message.back() == ' ')) message.pop_back();
    return message;
}

// A program started from this one and every program it starts in turn (2026-11-20), held in a
// job object: what is started inside it is in it from its first instruction, so the whole tree
// is ended in one step (TerminateJobObject), with nothing slipping out between finding a process
// and ending it, as a tree walked and killed one process at a time lets a fast chain do. With
// limits, the system holds the tree to them together (memory, and processes at once), and a
// thread waiting on the job's reports ends the tree as soon as it goes past one; and the tree is
// ended with this program, however it ends.
struct Tree {
    HANDLE job = nullptr;
    HANDLE port = nullptr;     // where the job reports; with limits only
    std::atomic<int> past{0};  // the limit it went past: 1 memory, 2 processes
    std::thread watcher;
    // Markers posted to the port by settle(), and the last the watcher has handled.
    std::atomic<uint64_t> markersPosted{0}, markersSeen{0};
    static constexpr DWORD kMarker = 0xFFFF0001;  // no job message has this number

    void end() { TerminateJobObject(job, 1); }

    // Waits until the watcher has handled everything the job reported before now (o-process-
    // limit-race, 2026-11-21). A program that goes past its memory limit can die of the
    // allocation refused at once, and its end be seen before the watcher has taken the job's
    // message: limitReached was then null (1 run in a suite's 600 under load). The port hands
    // its packets over in order, and the job's message is posted as the limit is hit, before the
    // program can end of it; so a marker posted now and seen by the watcher comes after it.
    void settle() {
        if (port == nullptr || !watcher.joinable()) return;
        const uint64_t n = ++markersPosted;
        if (!PostQueuedCompletionStatus(port, kMarker, static_cast<ULONG_PTR>(n), nullptr)) return;
        for (int i = 0; i < 5000 && markersSeen.load() < n; i++) Sleep(1);
    }
    ~Tree() {
        if (port != nullptr) CloseHandle(port);  // the watcher's wait returns
        if (watcher.joinable()) watcher.join();
        if (job != nullptr) CloseHandle(job);  // with limits, ends what is left of the tree
    }
};

// A job for a program about to start; null where the system makes none. `memory` (bytes) and
// `processes` above 0 are limits for the whole tree.
std::shared_ptr<Tree> makeTree(int64_t memory, int64_t processes) {
    auto tree = std::make_shared<Tree>();
    tree->job = CreateJobObjectW(nullptr, nullptr);
    if (tree->job == nullptr) return nullptr;
    if (memory <= 0 && processes <= 0) return tree;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (memory > 0) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit = static_cast<SIZE_T>(memory);
    }
    if (processes > 0) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = static_cast<DWORD>(std::min<int64_t>(processes, 0x7FFFFFFF));
    }
    tree->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT link{tree->job, tree->port};
    if (!SetInformationJobObject(tree->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) || tree->port == nullptr ||
        !SetInformationJobObject(tree->job, JobObjectAssociateCompletionPortInformation, &link, sizeof(link)))
        return nullptr;
    Tree* watched = tree.get();
    tree->watcher = std::thread([watched] {
        DWORD message = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;
        while (GetQueuedCompletionStatus(watched->port, &message, &key, &overlapped, INFINITE)) {
            if (message == Tree::kMarker) {
                watched->markersSeen.store(static_cast<uint64_t>(key));
                continue;
            }
            if (message == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT || message == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT) {
                watched->past = 1;
                watched->end();
            } else if (message == JOB_OBJECT_MSG_ACTIVE_PROCESS_LIMIT) {
                if (watched->past == 0) watched->past = 2;
                watched->end();
            }
        }
    });
    return tree;
}

// Something that can be read from, written to, waited for or closed: a file, one end of a
// pipe, a socket, a listening socket, a process. Programs know each by a number.
struct Stream {
    enum class Kind { File, Pipe, Socket, Listener, Process, Datagram };
    Kind kind;
    HANDLE handle = INVALID_HANDLE_VALUE;
    SOCKET socket = INVALID_SOCKET;
    // A Unix domain socket's file (2026-11-19), the one it connected to or listens on; a
    // listener that made the file removes it as it is closed.
    std::string unixPath;
    bool removesFile = false;
    // A socket's read timeout in milliseconds, 0 for none (as SO_RCVTIMEO holds it), for the wait
    // a Unix domain socket's read makes itself (waitReadable).
    std::atomic<int64_t> readLimitMs{0};
    DWORD pid = 0;
    // A process's job: it and what it starts, ended together (2026-11-20); null where there is none.
    std::shared_ptr<Tree> tree;

    std::atomic<bool> closed{false};
    std::atomic<int> users{0};  // threads inside a system call on it

    // Read side: what has been read from the system and not yet handed out.
    std::mutex readMutex;
    std::string buffer;
    size_t position = 0;
    bool ended = false;
    std::mutex writeMutex;
    // Read in Charset.utf16: whether its start has been looked at for a byte order mark, and
    // the order it says (G53); written in it, whether the mark has been.
    bool orderKnown = false;
    Charset readOrder = Charset::Utf16Be;
    bool markWritten = false;

    // A process that has ended, and what it ended with.
    std::atomic<bool> exited{false};
    std::atomic<int> exitCode{0};

    // A sandbox's file or pipe (M6.6.2): read, written and closed through it, not the system.
    std::shared_ptr<Channel> channel;
    // A sandbox's program, for a Process stream (M6.6.2 step 4).
    std::shared_ptr<SimulatedProcess> simulated;
    // A sandbox's port a program listens on, for a Listener (step 5).
    std::shared_ptr<SandboxListener> listener;

    explicit Stream(Kind k) : kind(k) {}

    ~Stream() { close(); }

    bool isSocket() const { return kind == Kind::Socket || kind == Kind::Listener || kind == Kind::Datagram; }
    // A socket's address family (AF_INET, AF_INET6), where this program made it; 0 where not known.
    int family = 0;

    // Closes it, first getting any thread that is waiting in it out.
    void close() {
        if (closed.exchange(true)) return;
        if (channel != nullptr) {
            channel->close();
            return;
        }
        if (listener != nullptr) {
            std::lock_guard<std::mutex> hold(listener->mutex);
            listener->closed = true;
            listener->changed.notify_all();
            return;
        }
        if (isSocket()) {
            if (socket != INVALID_SOCKET) {
                shutdown(socket, SD_BOTH);
                closesocket(socket);  // a thread in accept or recv returns with an error
                socket = INVALID_SOCKET;
            }
            if (removesFile) DeleteFileW(widen(unixPath).c_str());
            return;
        }
        tree.reset();  // with limits, what is left of the tree ends with it
        if (handle == INVALID_HANDLE_VALUE) return;
        while (users.load() > 0) {
            CancelIoEx(handle, nullptr);
            Sleep(1);
        }
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
};

struct Use {
    Stream& stream;
    explicit Use(Stream& s) : stream(s) { stream.users++; }
    ~Use() { stream.users--; }
};

// Waits until a Unix domain socket has something to read, its end, or an error (o-uds-hang,
// 2026-11-21). Windows' AF_UNIX can leave a recv that is already waiting asleep when the other side
// shuts down its sending: the end is never read, and the reader waits for ever (a raw Winsock
// program of a client reading to the end while its server sends two lines and shuts down: 22 of 24
// copies hung within 2,000 rounds). WSAPoll sees the end every time (none of 48,000 rounds even
// waited out a slice). So a Unix domain socket's read waits in WSAPoll first, in slices of 200 ms,
// in which a close from another thread and the read's time limit are seen; recv then has what
// there is. False with `result` filled in where it is closed or the time ran out.
bool waitReadable(Stream& s, Outcome& result) {
    const int64_t limit = s.readLimitMs.load();
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (s.closed.load()) {
            result.error = "the socket is closed";
            return false;
        }
        int slice = 200;
        if (limit > 0) {
            const int64_t left = limit - static_cast<int64_t>(GetTickCount64() - start);
            if (left <= 0) {
                result.error = "the read timed out";
                result.timedOut = true;
                return false;
            }
            slice = static_cast<int>(std::min<int64_t>(slice, left));
        }
        WSAPOLLFD p{};
        p.fd = s.socket;
        p.events = POLLRDNORM;
        if (WSAPoll(&p, 1, slice) != 0) return true;  // something to read, the end, or an error: recv says which
    }
}

// Reads up to `max` bytes. The count read; 0 at the end; -1 with `result.error` filled in.
int rawRead(Stream& s, char* out, size_t max, Outcome& result) {
    Use use(s);
    if (s.closed.load()) {
        result.error = "the stream is closed";
        return -1;
    }
    if (s.channel != nullptr) return s.channel->read(out, std::min<size_t>(max, 1 << 30), result.error, result.timedOut);
    if (s.isSocket()) {
        if (!s.unixPath.empty() && !waitReadable(s, result)) return -1;
        const int got = recv(s.socket, out, static_cast<int>(std::min<size_t>(max, 1 << 30)), 0);
        if (got >= 0) return got;
        const int code = WSAGetLastError();
        if (s.closed.load()) result.error = "the socket is closed";
        else if (code == WSAETIMEDOUT) {
            result.error = "the read timed out";
            result.timedOut = true;
        }
        else if (code == WSAECONNRESET || code == WSAECONNABORTED) return 0;  // the other side went away
        else result.error = "cannot read from the socket: " + systemMessage(static_cast<DWORD>(code));
        return -1;
    }
    DWORD got = 0;
    if (ReadFile(s.handle, out, static_cast<DWORD>(std::min<size_t>(max, 1 << 30)), &got, nullptr)) return static_cast<int>(got);
    const DWORD code = GetLastError();
    if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) return 0;  // the writer has closed its end
    result.error = code == ERROR_OPERATION_ABORTED || s.closed.load() ? "the stream is closed" : "cannot read: " + systemMessage(code);
    return -1;
}

bool rawWrite(Stream& s, const char* data, size_t size, std::string& error) {
    Use use(s);
    if (s.channel != nullptr) {
        if (s.closed.load()) {
            error = "the stream is closed";
            return false;
        }
        return s.channel->write(data, size, error);
    }
    while (size > 0) {
        if (s.closed.load()) {
            error = "the stream is closed";
            return false;
        }
        if (s.isSocket()) {
            const int sent = send(s.socket, data, static_cast<int>(std::min<size_t>(size, 1 << 30)), 0);
            if (sent == SOCKET_ERROR) {
                error = s.closed.load() ? "the socket is closed" : "cannot write to the socket: " + systemMessage(static_cast<DWORD>(WSAGetLastError()));
                return false;
            }
            data += sent;
            size -= static_cast<size_t>(sent);
        } else {
            DWORD written = 0;
            if (!WriteFile(s.handle, data, static_cast<DWORD>(std::min<size_t>(size, 1 << 30)), &written, nullptr)) {
                const DWORD code = GetLastError();
                error = code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA ? "the other end of the pipe is closed"
                        : code == ERROR_OPERATION_ABORTED || s.closed.load() ? "the stream is closed"
                        : "cannot write: " + systemMessage(code);
                return false;
            }
            data += written;
            size -= written;
        }
    }
    return true;
}

#else  // POSIX

// ---- POSIX: streams (G56) ----

// A program started from this one and every program it starts in turn (2026-11-20). These systems
// have no job object to hold a tree: it is found from each process's parent in /proc, and ended
// by stopping every process in it (SIGSTOP, so that none starts another), finding the tree again
// until nothing new is in it, and then killing them all (endTree). With limits, a thread looks at
// the limited trees ten times a second and ends one that has more processes, or more memory
// resident, than it may; and the limited trees are ended as this program ends normally.
struct Tree {
    int64_t memory = 0, processes = 0;
    std::atomic<int> past{0};  // the limit it went past: 1 memory, 2 processes
    // Nothing to wait for: the watcher here ends the tree itself, and says why before it does.
    void settle() {}
};

// What the system has to say about an error number.
std::string systemMessage(int code) {
    std::string message = std::strerror(code);
    while (!message.empty() && (message.back() == '.' || message.back() == ' ')) message.pop_back();
    return message;
}

// Something that can be read from, written to, waited for or closed: a file, one end of a
// pipe, a socket, a listening socket, a process. Programs know each by a number.
struct Stream {
    enum class Kind { File, Pipe, Socket, Listener, Process, Datagram };
    Kind kind;
    int fd = -1;
    // A Unix domain socket's file (2026-11-19), the one it connected to or listens on; a
    // listener that made the file removes it as it is closed.
    std::string unixPath;
    bool removesFile = false;
    pid_t pid = 0;
    // A process's limits and what it went past (2026-11-20); null for one started without limits.
    std::shared_ptr<Tree> tree;
    int readTimeout = 0;  // a socket's, in ms; 0 for none

    std::atomic<bool> closed{false};
    std::atomic<int> users{0};  // threads inside a system call on it

    // Read side: what has been read from the system and not yet handed out.
    std::mutex readMutex;
    std::string buffer;
    size_t position = 0;
    bool ended = false;
    std::mutex writeMutex;
    // Read in Charset.utf16: whether its start has been looked at for a byte order mark, and
    // the order it says (G53); written in it, whether the mark has been.
    bool orderKnown = false;
    Charset readOrder = Charset::Utf16Be;
    bool markWritten = false;

    // A process that has ended, and what it ended with; one waits for it at a time.
    std::atomic<bool> exited{false};
    std::atomic<int> exitCode{0};
    std::mutex waitMutex;

    // A sandbox's file or pipe (M6.6.2): read, written and closed through it, not the system.
    std::shared_ptr<Channel> channel;
    // A sandbox's program, for a Process stream (M6.6.2 step 4).
    std::shared_ptr<SimulatedProcess> simulated;
    // A sandbox's port a program listens on, for a Listener (step 5).
    std::shared_ptr<SandboxListener> listener;

    explicit Stream(Kind k) : kind(k) {}

    ~Stream() { close(); }

    bool isSocket() const { return kind == Kind::Socket || kind == Kind::Listener || kind == Kind::Datagram; }
    // A socket's address family (AF_INET, AF_INET6), where this program made it; 0 where not known.
    int family = 0;

    // Closes it, first getting any thread that is waiting in it out: one in a socket's read
    // returns at the shutdown, one waiting for something to read sees `closed` (waitReadable).
    void close() {
        if (closed.exchange(true)) return;
        if (channel != nullptr) {
            channel->close();
            return;
        }
        if (listener != nullptr) {
            std::lock_guard<std::mutex> hold(listener->mutex);
            listener->closed = true;
            listener->changed.notify_all();
            return;
        }
        if (fd < 0) return;
        if (kind == Kind::Socket) ::shutdown(fd, SHUT_RDWR);
        while (users.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ::close(fd);
        fd = -1;
        if (removesFile) ::unlink(unixPath.c_str());
    }
};

struct Use {
    Stream& stream;
    explicit Use(Stream& s) : stream(s) { stream.users++; }
    ~Use() { stream.users--; }
};

// Waits until the stream has something to read (or its end, or an error, which the read then
// says), or it is closed, or `timeout` ms have passed (-1 for no limit). In slices, so that a
// close() is seen at once, which on these systems does not wake a read already waiting.
bool waitReadable(Stream& s, int timeout, Outcome& result) {
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        if (s.closed.load()) {
            result.error = s.isSocket() ? "the socket is closed" : "the stream is closed";
            return false;
        }
        int slice = 100;
        if (timeout >= 0) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            if (spent >= timeout) {
                result.error = "the read timed out";
                result.timedOut = true;
                return false;
            }
            slice = static_cast<int>(std::min<int64_t>(slice, timeout - spent));
        }
        pollfd waiting{s.fd, POLLIN, 0};
        const int ready = ::poll(&waiting, 1, slice);
        if (ready > 0) return true;
        if (ready < 0 && errno != EINTR) {
            result.error = "cannot read: " + systemMessage(errno);
            return false;
        }
    }
}

// Reads up to `max` bytes. The count read; 0 at the end; -1 with `result.error` filled in.
int rawRead(Stream& s, char* out, size_t max, Outcome& result) {
    Use use(s);
    if (s.closed.load()) {
        result.error = "the stream is closed";
        return -1;
    }
    if (s.channel != nullptr) return s.channel->read(out, std::min<size_t>(max, 1 << 30), result.error, result.timedOut);
    // A file has something to read at once; anything else may wait.
    if (s.kind != Stream::Kind::File && !waitReadable(s, s.isSocket() && s.readTimeout > 0 ? s.readTimeout : -1, result)) return -1;
    const size_t wanted = std::min<size_t>(max, 1 << 30);
    for (;;) {
        const ssize_t got = s.isSocket() ? ::recv(s.fd, out, wanted, 0) : ::read(s.fd, out, wanted);
        if (got >= 0) return static_cast<int>(got);
        if (errno == EINTR) continue;
        if (s.isSocket() && (errno == ECONNRESET || errno == ECONNABORTED)) return 0;  // the other side went away
        if (s.closed.load()) result.error = s.isSocket() ? "the socket is closed" : "the stream is closed";
        else result.error = (s.isSocket() ? "cannot read from the socket: " : "cannot read: ") + systemMessage(errno);
        return -1;
    }
}

bool rawWrite(Stream& s, const char* data, size_t size, std::string& error) {
    Use use(s);
    if (s.channel != nullptr) {
        if (s.closed.load()) {
            error = "the stream is closed";
            return false;
        }
        return s.channel->write(data, size, error);
    }
    while (size > 0) {
        if (s.closed.load()) {
            error = s.isSocket() ? "the socket is closed" : "the stream is closed";
            return false;
        }
        const size_t wanted = std::min<size_t>(size, 1 << 30);
        const ssize_t written = s.isSocket() ? ::send(s.fd, data, wanted, 0) : ::write(s.fd, data, wanted);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (s.closed.load()) error = s.isSocket() ? "the socket is closed" : "the stream is closed";
            else if (s.isSocket()) error = "cannot write to the socket: " + systemMessage(errno);
            else error = errno == EPIPE ? "the other end of the pipe is closed" : "cannot write: " + systemMessage(errno);
            return false;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

#endif

// ---- The streams programs hold ----

// The streams that are open, by number. One table for the whole process: a worker has a VM
// and a lock of its own, so the table has its own lock too.
std::mutex streamsMutex;
std::unordered_map<int64_t, std::shared_ptr<Stream>> streams;
int64_t nextStream = 1;

// The handles of the program's own output and errors: never in the table, whose numbers start
// at 1, and written to as print writes (the write below), not through a Stream.
constexpr int64_t kStandardOutput = -1;
constexpr int64_t kStandardError = -2;

int64_t keep(std::shared_ptr<Stream> stream) {
    std::lock_guard<std::mutex> hold(streamsMutex);
    const int64_t id = nextStream++;
    streams.emplace(id, std::move(stream));
    return id;
}

// The stream of that number, or null; with `take`, it is taken out of the table as well.
std::shared_ptr<Stream> findStream(Value v, bool take = false) {
    std::lock_guard<std::mutex> hold(streamsMutex);
    const auto found = v.isInt() ? streams.find(v.as.i) : streams.end();
    if (found == streams.end()) return nullptr;
    std::shared_ptr<Stream> stream = found->second;
    if (take) streams.erase(found);
    return stream;
}

std::shared_ptr<Stream> streamOf(VM& vm, Value v) {
    std::shared_ptr<Stream> stream = findStream(v);
    if (stream == nullptr) throw ioError(vm, "the stream is closed");
    return stream;
}

Value bytesValue(VM& vm, const std::string& data) {
    ObjBytes* made = vm.alloc<ObjBytes>();
    made->data.assign(data.begin(), data.end());
    vm.account(made->data.size());
    return Value::object(made);
}

// More from the system into the stream's buffer. False at the end or on an error.
bool fill(Stream& s, Outcome& out) {
    if (s.ended) return false;
    if (s.position > 0) {
        // What has been handed out goes, so that the buffer holds only what is still to come.
        s.buffer.erase(0, s.position);
        s.position = 0;
    }
    const size_t before = s.buffer.size();
    s.buffer.resize(before + 64 * 1024);
    const int got = rawRead(s, s.buffer.data() + before, 64 * 1024, out);
    s.buffer.resize(before + static_cast<size_t>(got > 0 ? got : 0));
    if (got <= 0) s.ended = got == 0;
    return got > 0;
}

// The set a stream is read in: for Charset.utf16, the order the byte order mark at its start
// says, the mark passed over (G53). Called holding the read lock; false where reading failed.
bool readingOrder(Stream& s, Charset& cs, Outcome& out) {
    if (cs != Charset::Utf16) return true;
    if (!s.orderKnown) {
        while (s.buffer.size() - s.position < 2 && fill(s, out)) {}
        if (!out.error.empty()) return false;
        size_t mark = 0;
        s.readOrder = utf16Order(std::string_view(s.buffer).substr(s.position), Charset::Utf16Be, mark);
        s.position += mark;
        s.orderKnown = true;
    }
    cs = s.readOrder;
    return true;
}

// What spawn is asked for: a program and its arguments, where it runs, where its three
// standard streams go ("pipe", "inherit", "discard", "merge" for the error stream, or ">path",
// ">>path", "<path"), and changes to the environment (a map, or null).
struct Spawn {
    std::vector<std::string> command;
    std::string directory;
    std::string modes[3];
    Value environment;
    // Limits for the program and all it starts, together (2026-11-20): bytes of memory and processes at
    // once; 0 for none.
    int64_t memory = 0, processes = 0;
};

[[noreturn]] void badRedirect(VM& vm, const std::string& mode) {
    throw vm.runtimeError(Thrown::IllegalArgumentException, "a process's stream goes to pipe, inherit, discard, merge or a file, not '" + mode + "'");
}

#ifdef _WIN32

// ---- Windows: files, processes and sockets ----

// ---- Windows: a file read and written at a position (RandomAccessFile, M9/M10) ----

// Opened to read ("r") or to read and write ("rw", made if it is not there), others free to read
// and write it too, as Java's RandomAccessFile leaves it.
bool openRandom(const std::string& path, bool writable, Outcome& out) {
    HANDLE handle = CreateFileW(widen(path).c_str(), GENERIC_READ | (writable ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, writable ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        out.error = std::string(writable ? "cannot open file '" : "cannot read file '") + path + "': " + systemMessage(GetLastError());
        return false;
    }
    out.made = std::make_shared<Stream>(Stream::Kind::File);
    out.made->handle = handle;
    return true;
}

// Up to `count` bytes from `position`, fewer only at the end: how many, or -1 with `error`.
int64_t readAtPosition(Stream& s, int64_t position, char* into, size_t count, std::string& error) {
    Use use(s);
    size_t done = 0;
    while (done < count) {
        if (s.closed.load()) {
            error = "the file is closed";
            return -1;
        }
        OVERLAPPED at{};
        const uint64_t from = static_cast<uint64_t>(position) + done;
        at.Offset = static_cast<DWORD>(from);
        at.OffsetHigh = static_cast<DWORD>(from >> 32);
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(count - done, 1 << 30));
        if (!ReadFile(s.handle, into + done, want, &got, &at)) {
            const DWORD code = GetLastError();
            if (code == ERROR_HANDLE_EOF) break;
            error = "cannot read: " + systemMessage(code);
            return -1;
        }
        if (got == 0) break;
        done += got;
    }
    return static_cast<int64_t>(done);
}

bool writeAtPosition(Stream& s, int64_t position, const char* data, size_t count, std::string& error) {
    Use use(s);
    size_t done = 0;
    while (done < count) {
        if (s.closed.load()) {
            error = "the file is closed";
            return false;
        }
        OVERLAPPED at{};
        const uint64_t from = static_cast<uint64_t>(position) + done;
        at.Offset = static_cast<DWORD>(from);
        at.OffsetHigh = static_cast<DWORD>(from >> 32);
        DWORD wrote = 0;
        if (!WriteFile(s.handle, data + done, static_cast<DWORD>(std::min<size_t>(count - done, 1 << 30)), &wrote, &at)) {
            error = "cannot write: " + systemMessage(GetLastError());
            return false;
        }
        done += wrote;
    }
    return true;
}

int64_t sizeOfOpen(Stream& s, std::string& error) {
    Use use(s);
    LARGE_INTEGER size{};
    if (s.closed.load() || !GetFileSizeEx(s.handle, &size)) {
        error = s.closed.load() ? "the file is closed" : "cannot find the size: " + systemMessage(GetLastError());
        return -1;
    }
    return size.QuadPart;
}

// Makes the open file `size` bytes long: cut, or grown with zeros.
bool resizeOpen(Stream& s, int64_t size, std::string& error) {
    Use use(s);
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = size;
    if (s.closed.load() || !SetFileInformationByHandle(s.handle, FileEndOfFileInfo, &end, sizeof end)) {
        error = s.closed.load() ? "the file is closed" : "cannot set the size: " + systemMessage(GetLastError());
        return false;
    }
    return true;
}

bool syncOpen(Stream& s, std::string& error) {
    Use use(s);
    if (s.closed.load() || !FlushFileBuffers(s.handle)) {
        error = s.closed.load() ? "the file is closed" : "cannot sync: " + systemMessage(GetLastError());
        return false;
    }
    return true;
}

bool openFile(const std::string& path, const std::string& mode, Outcome& out) {
    const bool reading = mode == "r";
    HANDLE handle = CreateFileW(widen(path).c_str(), reading ? GENERIC_READ : (mode == "a" ? FILE_APPEND_DATA : GENERIC_WRITE),
                                FILE_SHARE_READ | (reading ? FILE_SHARE_WRITE : 0), nullptr,
                                reading ? OPEN_EXISTING : (mode == "a" ? OPEN_ALWAYS : CREATE_ALWAYS), FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        out.error = std::string(reading ? "cannot read file '" : "cannot write file '") + path + "': " + systemMessage(GetLastError());
        return false;
    }
    out.made = std::make_shared<Stream>(Stream::Kind::File);
    out.made->handle = handle;
    return true;
}

// One argument as it has to be written on a Windows command line for the program started to
// read it back unchanged.
void appendArgument(std::wstring& line, const std::wstring& argument) {
    if (!line.empty()) line += L' ';
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += argument;
        return;
    }
    line += L'"';
    for (size_t i = 0;; i++) {
        size_t backslashes = 0;
        while (i < argument.size() && argument[i] == L'\\') {
            i++;
            backslashes++;
        }
        if (i == argument.size()) {
            line.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[i] == L'"') line.append(backslashes * 2 + 1, L'\\');
        else line.append(backslashes, L'\\');
        line += argument[i];
    }
    line += L'"';
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    HANDLE release() {
        HANDLE out = h;
        h = INVALID_HANDLE_VALUE;
        return out;
    }
};

// Where one of a new process's three standard streams goes.
struct Redirect {
    Handle child;   // the end the new process gets
    Handle parent;  // the end this program keeps, for "pipe"
};

void openRedirect(VM& vm, const std::string& mode, bool childReads, DWORD standard, Redirect& out) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (mode == "pipe") {
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inheritable, 0)) throw ioError(vm, "cannot make a pipe: " + systemMessage(GetLastError()));
        out.child.h = childReads ? read : write;
        out.parent.h = childReads ? write : read;
        SetHandleInformation(out.parent.h, HANDLE_FLAG_INHERIT, 0);  // only the child's end goes to the child
    } else if (mode == "inherit") {
        HANDLE own = GetStdHandle(standard);
        if (own == nullptr || own == INVALID_HANDLE_VALUE) own = INVALID_HANDLE_VALUE;
        else if (!DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &out.child.h, 0, TRUE, DUPLICATE_SAME_ACCESS)) out.child.h = INVALID_HANDLE_VALUE;
        if (out.child.h == INVALID_HANDLE_VALUE) openRedirect(vm, "discard", childReads, standard, out);  // this program has none itself
    } else if (mode == "discard") {
        out.child.h = CreateFileW(L"NUL", childReads ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
    } else if (!mode.empty() && (mode[0] == '>' || mode[0] == '<')) {
        const bool append = mode.compare(0, 2, ">>") == 0;
        const std::wstring path = widen(mode.substr(append ? 2 : 1));
        out.child.h = mode[0] == '<' ? CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, &inheritable, OPEN_EXISTING, 0, nullptr)
                                     : CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                                                   append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out.child.h == INVALID_HANDLE_VALUE)
            throw ioError(vm, "cannot open '" + mode.substr(append ? 2 : 1) + "' for the process: " + systemMessage(GetLastError()));
    } else {
        badRedirect(vm, mode);
    }
}

// The environment of this program with `changes` made to it, as CreateProcess wants it.
std::wstring environmentBlock(VM& vm, Value changes) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    if (wchar_t* own = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = own; *entry != L'\0'; entry += wcslen(entry) + 1) {
            const std::wstring text(entry);
            const size_t equals = text.find(L'=', 1);  // names such as "=C:" start with one
            if (equals != std::wstring::npos) entries.emplace_back(text.substr(0, equals), text.substr(equals + 1));
        }
        FreeEnvironmentStringsW(own);
    }
    for (const ValueMap::Entry& change : asMap(changes)->map.entries()) {
        if (!change.live) continue;
        const std::wstring name = widen(vm.toStr(change.key, "environment name"));
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) { return _wcsicmp(entry.first.c_str(), name.c_str()) == 0; }),
                      entries.end());
        if (!change.value.isNull()) entries.emplace_back(name, widen(vm.stringify(change.value)));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    std::wstring block;
    for (const auto& entry : entries) {
        block += entry.first + L'=' + entry.second;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

// Starts the program: the process, then the ends of its streams this program keeps (null for
// those that are not "pipe").
std::array<std::shared_ptr<Stream>, 4> startProcess(VM& vm, const Spawn& spawn) {
    std::wstring commandLine;
    for (const std::string& part : spawn.command) appendArgument(commandLine, widen(part));
    const std::string& program = spawn.command[0];
    const std::wstring directory = widen(spawn.directory);
    const std::string& errorMode = spawn.modes[2];

    Redirect input, output, error;
    openRedirect(vm, spawn.modes[0], true, STD_INPUT_HANDLE, input);
    openRedirect(vm, spawn.modes[1], false, STD_OUTPUT_HANDLE, output);
    if (errorMode != "merge") openRedirect(vm, errorMode, false, STD_ERROR_HANDLE, error);
    std::wstring environment;
    if (spawn.environment.isObjType(ObjType::Map)) environment = environmentBlock(vm, spawn.environment);

    // The new process is given these three handles and no others, whatever else this
    // program has open at the moment.
    HANDLE inherited[3] = {input.child.h, output.child.h, errorMode == "merge" ? output.child.h : error.child.h};
    const DWORD handleCount = inherited[2] == inherited[1] ? 2 : 3;
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<char> attributeMemory(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeMemory.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, handleCount * sizeof(HANDLE), nullptr, nullptr))
        throw ioError(vm, "cannot start '" + program + "': " + systemMessage(GetLastError()));

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inherited[0];
    startup.StartupInfo.hStdOutput = inherited[1];
    startup.StartupInfo.hStdError = inherited[2];
    startup.lpAttributeList = attributes;

    // The job it and what it starts are held in; with limits, it must be had. The program starts
    // suspended, is put in the job, and only then runs, so nothing it starts is outside.
    std::shared_ptr<Tree> tree = makeTree(spawn.memory, spawn.processes);
    const bool limited = spawn.memory > 0 || spawn.processes > 0;
    if (limited && tree == nullptr) throw ioError(vm, "cannot start '" + program + "' within limits: " + systemMessage(GetLastError()));
    // A program without a console of its own does not make one appear for what it starts.
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | (GetConsoleWindow() == nullptr ? CREATE_NO_WINDOW : 0) |
                        (tree != nullptr ? CREATE_SUSPENDED : 0);
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> line(commandLine.begin(), commandLine.end());
    line.push_back(L'\0');
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, flags, environment.empty() ? nullptr : environment.data(),
                                        directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &info);
    const DWORD failure = started ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!started) throw ioError(vm, "cannot start '" + program + "': " + systemMessage(failure));
    if (tree != nullptr && !AssignProcessToJobObject(tree->job, info.hProcess)) {
        const DWORD why = GetLastError();
        if (limited) {
            TerminateProcess(info.hProcess, 1);
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
            throw ioError(vm, "cannot start '" + program + "' within limits: " + systemMessage(why));
        }
        tree = nullptr;  // ended alone, as before jobs
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);

    std::array<std::shared_ptr<Stream>, 4> made;
    made[0] = std::make_shared<Stream>(Stream::Kind::Process);
    made[0]->handle = info.hProcess;
    made[0]->pid = info.dwProcessId;
    made[0]->tree = std::move(tree);
    Redirect* ends[3] = {&input, &output, &error};
    for (int k = 0; k < 3; k++) {
        if (ends[k]->parent.h == INVALID_HANDLE_VALUE) continue;
        made[k + 1] = std::make_shared<Stream>(Stream::Kind::Pipe);
        made[k + 1]->handle = ends[k]->parent.release();
    }
    return made;
}

// Waits for a process to end, `limit` ms at most (-1 for as long as it takes): whether it
// ended, its exit code then kept.
bool waitProcess(Stream& s, int64_t limit) {
    if (s.simulated != nullptr) {
        int code = 0;
        if (!waitSimulated(*s.simulated, limit, code)) return false;
        s.exitCode = code;
        s.exited = true;
        return true;
    }
    if (s.exited.load()) return true;
    if (s.closed.load() || WaitForSingleObject(s.handle, limit < 0 ? INFINITE : static_cast<DWORD>(std::min<int64_t>(limit, 4000000000))) != WAIT_OBJECT_0)
        return false;
    DWORD code = 0;
    GetExitCodeProcess(s.handle, &code);
    s.exitCode = static_cast<int>(code);
    s.exited = true;
    return true;
}

// It and every program it started that is still running.
void killProcess(Stream& s) {
    if (s.simulated != nullptr) return endSimulated(*s.simulated, 1, true);
    if (s.tree != nullptr) s.tree->end();
    TerminateProcess(s.handle, 1);
}

int64_t currentProcessId() { return GetCurrentProcessId(); }

void startNetworking(VM& vm) {
    static std::mutex winsockMutex;
    static bool winsockStarted = false;
    std::lock_guard<std::mutex> hold(winsockMutex);
    if (winsockStarted) return;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw ioError(vm, "networking is not available");
    winsockStarted = true;
}

// A TCP connection, tried for at most `timeout` ms where that is not negative.
void connectTo(const std::string& host, int64_t port, int64_t timeout, Outcome& out) {
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfoW* found = nullptr;
    if (GetAddrInfoW(widen(host).c_str(), std::to_wstring(port).c_str(), &hints, &found) != 0 || found == nullptr) {
        out.error = "cannot find host '" + host + "'";
        out.unknownHost = true;
        return;
    }
    int lastError = 0;
    for (const addrinfoW* address = found; address != nullptr && out.made == nullptr; address = address->ai_next) {
        SOCKET s = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        bool connected;
        if (timeout < 0) {
            connected = ::connect(s, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0;
            if (!connected) lastError = WSAGetLastError();
        } else {
            // With a time limit: start connecting without waiting, then wait that long.
            u_long nonBlocking = 1;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            connected = ::connect(s, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0;
            if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set writable, failed;
                FD_ZERO(&writable);
                FD_ZERO(&failed);
                FD_SET(s, &writable);
                FD_SET(s, &failed);
                timeval limit{static_cast<long>(timeout / 1000), static_cast<long>((timeout % 1000) * 1000)};
                const int ready = select(0, nullptr, &writable, &failed, &limit);
                connected = ready > 0 && FD_ISSET(s, &writable);
                lastError = ready == 0 ? WSAETIMEDOUT : WSAECONNREFUSED;
            }
            nonBlocking = 0;
            ioctlsocket(s, FIONBIO, &nonBlocking);
        }
        if (!connected) {
            closesocket(s);
            continue;
        }
        out.made = std::make_shared<Stream>(Stream::Kind::Socket);
        out.made->socket = s;
    }
    FreeAddrInfoW(found);
    if (out.made == nullptr)
        out.error = "cannot connect to " + host + ":" + std::to_string(port) + ": " +
                    (lastError == WSAETIMEDOUT ? "timed out" : systemMessage(static_cast<DWORD>(lastError)));
}

void acceptOn(Stream& s, Outcome& out) {
    Use use(s);
    const SOCKET connection = ::accept(s.socket, nullptr, nullptr);
    if (connection == INVALID_SOCKET) {
        out.error = s.closed.load() ? "the socket is closed" : "cannot accept a connection: " + systemMessage(static_cast<DWORD>(WSAGetLastError()));
        return;
    }
    out.made = std::make_shared<Stream>(Stream::Kind::Socket);
    out.made->socket = connection;
    out.made->unixPath = s.unixPath;
}

// The local port, and the remote address and port, of a socket; false for one it cannot say.
bool localPort(Stream& s, int& port) {
    if (!s.unixPath.empty()) return false;  // a Unix domain socket: a file, no port
    sockaddr_storage address{};
    int size = sizeof(address);
    if (getsockname(s.socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    port = ntohs(address.ss_family == AF_INET6 ? reinterpret_cast<const sockaddr_in6&>(address).sin6_port : reinterpret_cast<const sockaddr_in&>(address).sin_port);
    return true;
}
bool remoteEnd(Stream& s, std::string& host, int& port) {
    if (!s.unixPath.empty()) return false;
    sockaddr_storage address{};
    int size = sizeof(address);
    if (getpeername(s.socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    wchar_t text[64] = {};
    const void* raw = address.ss_family == AF_INET6 ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6&>(address).sin6_addr)
                                                    : static_cast<const void*>(&reinterpret_cast<const sockaddr_in&>(address).sin_addr);
    InetNtopW(address.ss_family, raw, text, 64);
    host = narrow(text);
    port = ntohs(address.ss_family == AF_INET6 ? reinterpret_cast<const sockaddr_in6&>(address).sin6_port : reinterpret_cast<const sockaddr_in&>(address).sin_port);
    return true;
}

void setReadTimeout(Stream& s, int64_t limit) {
    const DWORD ms = static_cast<DWORD>(limit);
    s.readLimitMs.store(limit);
    setsockopt(s.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

void shutdownOutput(Stream& s) { shutdown(s.socket, SD_SEND); }

// ---- Unix domain sockets (2026-11-19): a file of the file system as the address ----

// The address of the socket file `path`, false (and why) where the path will not fit.
bool unixAddress(const std::string& path, sockaddr_un& address, std::string& why) {
    address = sockaddr_un{};
    address.sun_family = AF_UNIX;
    if (path.empty()) {
        why = "a Unix domain socket's path is empty";
        return false;
    }
    if (path.size() >= sizeof(address.sun_path)) {
        why = "a Unix domain socket's path is at most " + std::to_string(sizeof(address.sun_path) - 1) + " bytes, not " + std::to_string(path.size());
        return false;
    }
    std::memcpy(address.sun_path, path.data(), path.size());
    return true;
}
// Why a Unix domain socket could not be made, where the system has none.
std::string unixMissing(int error) {
    return error == WSAEAFNOSUPPORT ? "Unix domain sockets need Windows 10 (1803) or later" : systemMessage(static_cast<DWORD>(error));
}

// A connection to the socket file `path`, tried for at most `timeout` ms where that is not
// negative.
void connectUnix(const std::string& path, int64_t timeout, Outcome& out) {
    sockaddr_un address;
    if (!unixAddress(path, address, out.error)) return;
    SOCKET s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        out.error = "cannot connect to " + path + ": " + unixMissing(WSAGetLastError());
        return;
    }
    bool connected;
    int lastError = 0;
    if (timeout < 0) {
        connected = ::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        if (!connected) lastError = WSAGetLastError();
    } else {
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        connected = ::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        if (!connected) lastError = WSAGetLastError();
        if (!connected && lastError == WSAEWOULDBLOCK) {
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval limit{static_cast<long>(timeout / 1000), static_cast<long>((timeout % 1000) * 1000)};
            const int ready = select(0, nullptr, &writable, &failed, &limit);
            connected = ready > 0 && FD_ISSET(s, &writable);
            lastError = ready == 0 ? WSAETIMEDOUT : WSAECONNREFUSED;
        }
        nonBlocking = 0;
        ioctlsocket(s, FIONBIO, &nonBlocking);
    }
    if (!connected) {
        closesocket(s);
        out.error = "cannot connect to " + path + ": " + (lastError == WSAETIMEDOUT ? "timed out" : systemMessage(static_cast<DWORD>(lastError)));
        return;
    }
    out.made = std::make_shared<Stream>(Stream::Kind::Socket);
    out.made->socket = s;
    out.made->unixPath = path;
}

// A socket that takes connections at the socket file `path`, made now (an IOException where
// a file of that name is there already); closed, it removes the file.
std::shared_ptr<Stream> listenUnix(VM& vm, const std::string& path, int backlog) {
    sockaddr_un address;
    std::string why;
    if (!unixAddress(path, address, why)) throw ioError(vm, why);
    SOCKET s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) throw ioError(vm, "cannot listen on " + path + ": " + unixMissing(WSAGetLastError()));
    if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(s, backlog) != 0) {
        const int error = WSAGetLastError();
        closesocket(s);
        throw ioError(vm, "cannot listen on " + path + ": " +
                              (error == WSAEADDRINUSE ? std::string("a file of that name is there already") : systemMessage(static_cast<DWORD>(error))));
    }
    auto listener = std::make_shared<Stream>(Stream::Kind::Listener);
    listener->socket = s;
    listener->unixPath = path;
    listener->removesFile = true;
    return listener;
}

#else  // POSIX

// ---- POSIX: files, processes and sockets (G56) ----

// ---- POSIX: a file read and written at a position (RandomAccessFile, M9/M10) ----

bool openRandom(const std::string& path, bool writable, Outcome& out) {
    const int fd = ::open(path.c_str(), (writable ? O_RDWR | O_CREAT : O_RDONLY) | O_CLOEXEC, 0666);
    if (fd < 0) {
        out.error = std::string(writable ? "cannot open file '" : "cannot read file '") + path + "': " + std::strerror(errno);
        return false;
    }
    out.made = std::make_shared<Stream>(Stream::Kind::File);
    out.made->fd = fd;
    return true;
}

int64_t readAtPosition(Stream& s, int64_t position, char* into, size_t count, std::string& error) {
    Use use(s);
    size_t done = 0;
    while (done < count) {
        if (s.closed.load()) {
            error = "the file is closed";
            return -1;
        }
        const ssize_t got = ::pread(s.fd, into + done, std::min<size_t>(count - done, 1 << 30), static_cast<off_t>(position + static_cast<int64_t>(done)));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            error = std::string("cannot read: ") + std::strerror(errno);
            return -1;
        }
        if (got == 0) break;
        done += static_cast<size_t>(got);
    }
    return static_cast<int64_t>(done);
}

bool writeAtPosition(Stream& s, int64_t position, const char* data, size_t count, std::string& error) {
    Use use(s);
    size_t done = 0;
    while (done < count) {
        if (s.closed.load()) {
            error = "the file is closed";
            return false;
        }
        const ssize_t wrote = ::pwrite(s.fd, data + done, std::min<size_t>(count - done, 1 << 30), static_cast<off_t>(position + static_cast<int64_t>(done)));
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote < 0) {
            error = std::string("cannot write: ") + std::strerror(errno);
            return false;
        }
        done += static_cast<size_t>(wrote);
    }
    return true;
}

int64_t sizeOfOpen(Stream& s, std::string& error) {
    Use use(s);
    struct stat info {};
    if (s.closed.load() || ::fstat(s.fd, &info) != 0) {
        error = s.closed.load() ? "the file is closed" : std::string("cannot find the size: ") + std::strerror(errno);
        return -1;
    }
    return static_cast<int64_t>(info.st_size);
}

// Makes the open file `size` bytes long: cut, or grown with zeros.
bool resizeOpen(Stream& s, int64_t size, std::string& error) {
    Use use(s);
    if (s.closed.load() || ::ftruncate(s.fd, static_cast<off_t>(size)) != 0) {
        error = s.closed.load() ? "the file is closed" : std::string("cannot set the size: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool syncOpen(Stream& s, std::string& error) {
    Use use(s);
    if (s.closed.load() || ::fsync(s.fd) != 0) {
        error = s.closed.load() ? "the file is closed" : std::string("cannot sync: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool openFile(const std::string& path, const std::string& mode, Outcome& out) {
    const bool reading = mode == "r";
    const int flags = reading ? O_RDONLY : O_WRONLY | O_CREAT | (mode == "a" ? O_APPEND : O_TRUNC);
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC, 0666);
    if (fd < 0) {
        out.error = std::string(reading ? "cannot read file '" : "cannot write file '") + path + "': " + systemMessage(errno);
        return false;
    }
    out.made = std::make_shared<Stream>(Stream::Kind::File);
    out.made->fd = fd;
    return true;
}

// The file descriptors a spawn opens, closed when it is done with them.
struct Descriptors {
    std::vector<int> open;
    ~Descriptors() {
        for (int fd : open) ::close(fd);
    }
    int add(int fd) {
        open.push_back(fd);
        return fd;
    }
    int release(int fd) {
        open.erase(std::remove(open.begin(), open.end(), fd), open.end());
        return fd;
    }
};

// Starts the program: the process, then the ends of its streams this program keeps (null for
// those that are not "pipe"). Every descriptor this program opens is closed in what it starts
// (O_CLOEXEC), but for the three it is given.
// A process started with limits: watched from then on (below).
void watch(const std::shared_ptr<Stream>& s);

std::array<std::shared_ptr<Stream>, 4> startProcess(VM& vm, const Spawn& spawn) {
    Descriptors fds;
    int child[3] = {-1, -1, -1}, parent[3] = {-1, -1, -1};
    for (int k = 0; k < 3; k++) {
        const std::string& mode = spawn.modes[k];
        const bool childReads = k == 0;
        if (mode == "pipe") {
            int ends[2];
            if (::pipe(ends) != 0) throw ioError(vm, "cannot make a pipe: " + systemMessage(errno));
            for (int end : ends) ::fcntl(fds.add(end), F_SETFD, FD_CLOEXEC);
            child[k] = childReads ? ends[0] : ends[1];
            parent[k] = childReads ? ends[1] : ends[0];
        } else if (mode == "inherit" || (k == 2 && mode == "merge")) {
            // The same as this program's, or (merge) its output.
        } else if (mode == "discard") {
            child[k] = fds.add(::open("/dev/null", (childReads ? O_RDONLY : O_WRONLY) | O_CLOEXEC));
        } else if (!mode.empty() && (mode[0] == '>' || mode[0] == '<')) {
            const bool append = mode.compare(0, 2, ">>") == 0;
            const std::string path = mode.substr(append ? 2 : 1);
            const int fd = mode[0] == '<' ? ::open(path.c_str(), O_RDONLY | O_CLOEXEC)
                                          : ::open(path.c_str(), O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC) | O_CLOEXEC, 0666);
            if (fd < 0) throw ioError(vm, "cannot open '" + path + "' for the process: " + systemMessage(errno));
            child[k] = fds.add(fd);
        } else {
            badRedirect(vm, mode);
        }
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int k = 0; k < 3; k++)
        if (child[k] >= 0) posix_spawn_file_actions_adddup2(&actions, child[k], k);
    if (spawn.modes[2] == "merge") posix_spawn_file_actions_adddup2(&actions, 1, 2);
    if (!spawn.directory.empty()) posix_spawn_file_actions_addchdir_np(&actions, spawn.directory.c_str());
    // What this program ignores (SIGPIPE, below) the new one does not.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);

    std::vector<char*> argv;
    for (const std::string& part : spawn.command) argv.push_back(const_cast<char*>(part.c_str()));
    argv.push_back(nullptr);
    // This program's environment, with the changes asked for made to it.
    std::vector<std::string> entries;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; entry++) entries.emplace_back(*entry);
    if (spawn.environment.isObjType(ObjType::Map)) {
        for (const ValueMap::Entry& change : asMap(spawn.environment)->map.entries()) {
            if (!change.live) continue;
            const std::string name = vm.toStr(change.key, "environment name");
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::string& entry) { return entry.compare(0, name.size() + 1, name + "=") == 0; }),
                          entries.end());
            if (!change.value.isNull()) entries.push_back(name + "=" + vm.stringify(change.value));
        }
    }
    std::vector<char*> envp;
    for (std::string& entry : entries) envp.push_back(entry.data());
    envp.push_back(nullptr);

    pid_t pid = 0;
    const int failure = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (failure != 0) throw ioError(vm, "cannot start '" + spawn.command[0] + "': " + systemMessage(failure));

    std::array<std::shared_ptr<Stream>, 4> made;
    made[0] = std::make_shared<Stream>(Stream::Kind::Process);
    made[0]->pid = pid;
    if (spawn.memory > 0 || spawn.processes > 0) {
        made[0]->tree = std::make_shared<Tree>();
        made[0]->tree->memory = spawn.memory;
        made[0]->tree->processes = spawn.processes;
        watch(made[0]);
    }
    for (int k = 0; k < 3; k++) {
        if (parent[k] < 0) continue;
        made[k + 1] = std::make_shared<Stream>(Stream::Kind::Pipe);
        made[k + 1]->fd = fds.release(parent[k]);
    }
    return made;  // the child's ends close here, in this program
}

// Waits for a process to end, `limit` ms at most (-1 for as long as it takes): whether it
// ended, its exit code then kept (128 and the signal's number for one a signal ended, as a
// shell says).
bool waitProcess(Stream& s, int64_t limit) {
    if (s.simulated != nullptr) {
        int code = 0;
        if (!waitSimulated(*s.simulated, limit, code)) return false;
        s.exitCode = code;
        s.exited = true;
        return true;
    }
    const auto started = std::chrono::steady_clock::now();
    int pause = 1;
    for (;;) {
        {
            std::lock_guard<std::mutex> hold(s.waitMutex);
            if (s.exited.load()) return true;
            int status = 0;
            const pid_t done = ::waitpid(s.pid, &status, WNOHANG);
            if (done == s.pid) {
                s.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
                s.exited = true;
                return true;
            }
            if (done < 0 && errno != EINTR) {
                s.exitCode = -1;  // not this program's to wait for any more
                s.exited = true;
                return true;
            }
        }
        if (s.closed.load()) return false;
        if (limit >= 0 && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(limit)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(pause));
        pause = std::min(pause * 2, 10);
    }
}

// Every process's children, by its parent's number in /proc/<n>/stat (the field after the
// name, whose closing bracket is the last in the line).
std::unordered_map<pid_t, std::vector<pid_t>> processChildren() {
    std::unordered_map<pid_t, std::vector<pid_t>> children;
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return children;
    while (const dirent* entry = ::readdir(proc)) {
        char* end = nullptr;
        const long pid = std::strtol(entry->d_name, &end, 10);
        if (pid <= 0 || *end != '\0') continue;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char text[512];
        const ssize_t got = ::read(fd, text, sizeof(text) - 1);
        ::close(fd);
        if (got <= 0) continue;
        text[got] = '\0';
        const char* close = std::strrchr(text, ')');
        if (close == nullptr) continue;
        char state = 0;
        long parent = 0;
        if (std::sscanf(close + 1, " %c %ld", &state, &parent) == 2) children[static_cast<pid_t>(parent)].push_back(static_cast<pid_t>(pid));
    }
    ::closedir(proc);
    return children;
}

// The process and all below it.
std::vector<pid_t> treeFrom(pid_t root, const std::unordered_map<pid_t, std::vector<pid_t>>& children) {
    std::vector<pid_t> tree{root};
    for (size_t i = 0; i < tree.size(); i++) {
        auto found = children.find(tree[i]);
        if (found != children.end()) tree.insert(tree.end(), found->second.begin(), found->second.end());
    }
    return tree;
}

// The memory the processes hold resident, in bytes.
int64_t residentBytes(const std::vector<pid_t>& pids) {
    static const int64_t page = ::sysconf(_SC_PAGESIZE);
    int64_t total = 0;
    for (pid_t pid : pids) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/statm", static_cast<int>(pid));
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char text[128];
        const ssize_t got = ::read(fd, text, sizeof(text) - 1);
        ::close(fd);
        if (got <= 0) continue;
        text[got] = '\0';
        long size = 0, resident = 0;
        if (std::sscanf(text, "%ld %ld", &size, &resident) == 2) total += resident * page;
    }
    return total;
}

// Ends the process and everything it started: each stopped as it is found, so none starts
// another, until a look finds nothing new; then all killed. Held under waitMutex, so that the
// process, not yet waited for, keeps its number throughout.
void endTree(Stream& s) {
    std::lock_guard<std::mutex> hold(s.waitMutex);
    if (s.exited.load()) return;
    std::vector<pid_t> stopped;
    for (int look = 0; look < 100; look++) {
        bool found = false;
        for (pid_t pid : treeFrom(s.pid, processChildren())) {
            if (std::find(stopped.begin(), stopped.end(), pid) != stopped.end()) continue;
            ::kill(pid, SIGSTOP);
            stopped.push_back(pid);
            found = true;
        }
        if (!found) break;
    }
    for (pid_t pid : stopped) ::kill(pid, SIGKILL);
}

// The processes started with limits, looked at ten times a second, and ended with this program.
std::mutex& limitedMutex() {
    static std::mutex* made = new std::mutex;
    return *made;
}
std::vector<std::weak_ptr<Stream>>& limited() {
    static auto* made = new std::vector<std::weak_ptr<Stream>>;
    return *made;
}

void watchLimited() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::vector<std::shared_ptr<Stream>> now;
        {
            std::lock_guard<std::mutex> hold(limitedMutex());
            auto& all = limited();
            all.erase(std::remove_if(all.begin(), all.end(),
                                     [](const std::weak_ptr<Stream>& w) {
                                         auto s = w.lock();
                                         return s == nullptr || s->exited.load();
                                     }),
                      all.end());
            for (auto& w : all)
                if (auto s = w.lock()) now.push_back(s);
        }
        if (now.empty()) continue;
        const auto children = processChildren();
        for (const auto& s : now) {
            const std::vector<pid_t> tree = treeFrom(s->pid, children);
            const Tree& t = *s->tree;
            int past = 0;
            if (t.processes > 0 && static_cast<int64_t>(tree.size()) > t.processes) past = 2;
            else if (t.memory > 0 && residentBytes(tree) > t.memory) past = 1;
            if (past == 0) continue;
            s->tree->past = past;
            endTree(*s);
        }
    }
}

// A process started with limits: watched from now on.
void watch(const std::shared_ptr<Stream>& s) {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread(watchLimited).detach();
        std::atexit([] {
            std::vector<std::shared_ptr<Stream>> left;
            {
                std::lock_guard<std::mutex> hold(limitedMutex());
                for (auto& w : limited())
                    if (auto s = w.lock()) left.push_back(s);
            }
            for (const auto& s : left) endTree(*s);
        });
    });
    std::lock_guard<std::mutex> hold(limitedMutex());
    limited().push_back(s);
}

// It and every program it started that is still running. Only one not yet seen to end: once it
// has been waited for, its number may be another's.
void killProcess(Stream& s) {
    if (s.simulated != nullptr) return endSimulated(*s.simulated, 1, true);
    endTree(s);
}

int64_t currentProcessId() { return ::getpid(); }

void startNetworking(VM&) {}

// A socket closed in what this program starts.
int newSocket(int family, int type, int protocol) {
    const int fd = ::socket(family, type, protocol);
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

// A TCP connection, tried for at most `timeout` ms where that is not negative.
void connectTo(const std::string& host, int64_t port, int64_t timeout, Outcome& out) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || found == nullptr) {
        out.error = "cannot find host '" + host + "'";
        out.unknownHost = true;
        return;
    }
    int lastError = 0;
    bool timedOut = false;
    for (const addrinfo* address = found; address != nullptr && out.made == nullptr; address = address->ai_next) {
        const int fd = newSocket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        bool connected;
        if (timeout < 0) {
            connected = ::connect(fd, address->ai_addr, address->ai_addrlen) == 0;
            if (!connected) lastError = errno;
        } else {
            // With a time limit: start connecting without waiting, then wait that long.
            const int flags = ::fcntl(fd, F_GETFL);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            connected = ::connect(fd, address->ai_addr, address->ai_addrlen) == 0;
            if (!connected && errno == EINPROGRESS) {
                pollfd waiting{fd, POLLOUT, 0};
                const int ready = ::poll(&waiting, 1, static_cast<int>(std::min<int64_t>(timeout, 2000000000)));
                int failure = 0;
                socklen_t size = sizeof(failure);
                if (ready > 0) ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &size);
                connected = ready > 0 && failure == 0;
                timedOut = ready == 0;
                lastError = ready > 0 ? failure : errno;
            } else if (!connected) {
                lastError = errno;
            }
            ::fcntl(fd, F_SETFL, flags);
        }
        if (!connected) {
            ::close(fd);
            continue;
        }
        out.made = std::make_shared<Stream>(Stream::Kind::Socket);
        out.made->fd = fd;
    }
    ::freeaddrinfo(found);
    if (out.made == nullptr)
        out.error = "cannot connect to " + host + ":" + std::to_string(port) + ": " + (timedOut ? "timed out" : systemMessage(lastError));
}

void acceptOn(Stream& s, Outcome& out) {
    Use use(s);
    if (!waitReadable(s, -1, out)) return;
    const int connection = ::accept(s.fd, nullptr, nullptr);
    if (connection < 0) {
        out.error = s.closed.load() ? "the socket is closed" : "cannot accept a connection: " + systemMessage(errno);
        return;
    }
    ::fcntl(connection, F_SETFD, FD_CLOEXEC);
    out.made = std::make_shared<Stream>(Stream::Kind::Socket);
    out.made->fd = connection;
    out.made->unixPath = s.unixPath;
}

int portOf(const sockaddr_storage& address) {
    return ntohs(address.ss_family == AF_INET6 ? reinterpret_cast<const sockaddr_in6&>(address).sin6_port : reinterpret_cast<const sockaddr_in&>(address).sin_port);
}

// The local port, and the remote address and port, of a socket; false for one it cannot say.
bool localPort(Stream& s, int& port) {
    if (!s.unixPath.empty()) return false;  // a Unix domain socket: a file, no port
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    if (::getsockname(s.fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    port = portOf(address);
    return true;
}
bool remoteEnd(Stream& s, std::string& host, int& port) {
    if (!s.unixPath.empty()) return false;
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    if (::getpeername(s.fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    char text[64] = {};
    const void* raw = address.ss_family == AF_INET6 ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6&>(address).sin6_addr)
                                                    : static_cast<const void*>(&reinterpret_cast<const sockaddr_in&>(address).sin_addr);
    ::inet_ntop(address.ss_family, raw, text, sizeof(text));
    host = text;
    port = portOf(address);
    return true;
}

void setReadTimeout(Stream& s, int64_t limit) { s.readTimeout = static_cast<int>(std::min<int64_t>(limit, 2000000000)); }

void shutdownOutput(Stream& s) { ::shutdown(s.fd, SHUT_WR); }

// ---- Unix domain sockets (2026-11-19): a file of the file system as the address ----

// The address of the socket file `path`, false (and why) where the path will not fit.
bool unixAddress(const std::string& path, sockaddr_un& address, std::string& why) {
    address = sockaddr_un{};
    address.sun_family = AF_UNIX;
    if (path.empty()) {
        why = "a Unix domain socket's path is empty";
        return false;
    }
    if (path.size() >= sizeof(address.sun_path)) {
        why = "a Unix domain socket's path is at most " + std::to_string(sizeof(address.sun_path) - 1) + " bytes, not " + std::to_string(path.size());
        return false;
    }
    std::memcpy(address.sun_path, path.data(), path.size());
    return true;
}

// A connection to the socket file `path`, tried for at most `timeout` ms where that is not
// negative.
void connectUnix(const std::string& path, int64_t timeout, Outcome& out) {
    sockaddr_un address;
    if (!unixAddress(path, address, out.error)) return;
    const int fd = newSocket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        out.error = "cannot connect to " + path + ": " + systemMessage(errno);
        return;
    }
    bool connected;
    int lastError = 0;
    bool timedOut = false;
    if (timeout < 0) {
        connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        if (!connected) lastError = errno;
    } else {
        const int flags = ::fcntl(fd, F_GETFL);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        if (!connected && (errno == EINPROGRESS || errno == EAGAIN)) {
            pollfd waiting{fd, POLLOUT, 0};
            const int ready = ::poll(&waiting, 1, static_cast<int>(std::min<int64_t>(timeout, 2000000000)));
            int failure = 0;
            socklen_t size = sizeof(failure);
            if (ready > 0) ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &size);
            connected = ready > 0 && failure == 0;
            timedOut = ready == 0;
            lastError = ready > 0 ? failure : errno;
        } else if (!connected) {
            lastError = errno;
        }
        ::fcntl(fd, F_SETFL, flags);
    }
    if (!connected) {
        ::close(fd);
        out.error = "cannot connect to " + path + ": " + (timedOut ? "timed out" : systemMessage(lastError));
        return;
    }
    out.made = std::make_shared<Stream>(Stream::Kind::Socket);
    out.made->fd = fd;
    out.made->unixPath = path;
}

// A socket that takes connections at the socket file `path`, made now (an IOException where
// a file of that name is there already); closed, it removes the file.
std::shared_ptr<Stream> listenUnix(VM& vm, const std::string& path, int backlog) {
    sockaddr_un address;
    std::string why;
    if (!unixAddress(path, address, why)) throw ioError(vm, why);
    const int fd = newSocket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd, backlog) != 0) {
        const int error = errno;
        if (fd >= 0) ::close(fd);
        throw ioError(vm, "cannot listen on " + path + ": " + (error == EADDRINUSE ? std::string("a file of that name is there already") : systemMessage(error)));
    }
    auto listener = std::make_shared<Stream>(Stream::Kind::Listener);
    listener->fd = fd;
    listener->unixPath = path;
    listener->removesFile = true;
    return listener;
}

#endif

// ---- Addresses, names and datagrams (networking plan, part 1), for both kinds of system ----

#ifdef _WIN32
using RawSocket = SOCKET;
const RawSocket noSocket = INVALID_SOCKET;
using AddressSize = int;
RawSocket& rawOf(Stream& s) { return s.socket; }
int lastNetError() { return WSAGetLastError(); }
std::string netMessage(int code) { return systemMessage(static_cast<DWORD>(code)); }
void closeRaw(RawSocket s) { closesocket(s); }
RawSocket newRawSocket(int family, int type) { return ::socket(family, type, 0); }
const int timedOutError = WSAETIMEDOUT;
#else
using RawSocket = int;
const RawSocket noSocket = -1;
using AddressSize = socklen_t;
RawSocket& rawOf(Stream& s) { return s.fd; }
int lastNetError() { return errno; }
std::string netMessage(int code) { return systemMessage(code); }
void closeRaw(RawSocket s) { ::close(s); }
RawSocket newRawSocket(int family, int type) { return newSocket(family, type, 0); }
const int timedOutError = ETIMEDOUT;
#endif

// The address written in `text` (IPv4's dotted form or IPv6's, or "localhost") with `port`;
// false where the text is no address.
bool socketAddressOf(const std::string& text, int64_t port, sockaddr_storage& out, AddressSize& size) {
    out = sockaddr_storage{};
    const std::string literal = text == "localhost" ? "127.0.0.1" : text;
    auto& v4 = reinterpret_cast<sockaddr_in&>(out);
    if (inet_pton(AF_INET, literal.c_str(), &v4.sin_addr) == 1) {
        v4.sin_family = AF_INET;
        v4.sin_port = htons(static_cast<uint16_t>(port));
        size = sizeof(sockaddr_in);
        return true;
    }
    auto& v6 = reinterpret_cast<sockaddr_in6&>(out);
    if (inet_pton(AF_INET6, literal.c_str(), &v6.sin6_addr) == 1) {
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(static_cast<uint16_t>(port));
        size = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

// An IPv4 address as IPv6 holds it (::ffff:a.b.c.d), for a socket that serves both.
void mapToIpv6(sockaddr_storage& address, AddressSize& size) {
    if (address.ss_family != AF_INET) return;
    const sockaddr_in v4 = reinterpret_cast<const sockaddr_in&>(address);
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = v4.sin_port;
    auto* bytes = reinterpret_cast<uint8_t*>(&v6.sin6_addr);
    bytes[10] = 0xff;
    bytes[11] = 0xff;
    std::memcpy(bytes + 12, &v4.sin_addr, 4);
    address = sockaddr_storage{};
    std::memcpy(&address, &v6, sizeof(v6));
    size = sizeof(sockaddr_in6);
}

// The text of an address, an IPv4 one that IPv6 holds (::ffff:a.b.c.d) given as IPv4; and its port.
std::string addressText(const sockaddr_storage& address, int* port = nullptr) {
    char text[INET6_ADDRSTRLEN] = {};
    if (address.ss_family == AF_INET6) {
        const auto& v6 = reinterpret_cast<const sockaddr_in6&>(address);
        if (port != nullptr) *port = ntohs(v6.sin6_port);
        const auto* bytes = reinterpret_cast<const uint8_t*>(&v6.sin6_addr);
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::memcmp(bytes, mapped, 12) == 0) inet_ntop(AF_INET, const_cast<uint8_t*>(bytes + 12), text, sizeof(text));
        else inet_ntop(AF_INET6, const_cast<in6_addr*>(&v6.sin6_addr), text, sizeof(text));
    } else if (address.ss_family == AF_INET) {
        const auto& v4 = reinterpret_cast<const sockaddr_in&>(address);
        if (port != nullptr) *port = ntohs(v4.sin_port);
        inet_ntop(AF_INET, const_cast<in_addr*>(&v4.sin_addr), text, sizeof(text));
    }
    return text;
}

// A socket of `type` bound to `address` and `port`. With no address, IPv6's "any" taking IPv4
// as well, one socket for both (IPV6_V6ONLY off), or IPv4's alone where the system has no IPv6;
// a given address is that address's family alone. noSocket, with `error` filled in, where it
// cannot be made.
RawSocket boundSocket(const std::string& address, int64_t port, int type, std::string& error) {
    sockaddr_storage local{};
    AddressSize size = 0;
    const bool both = address.empty();
    if (both) {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(local);
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(static_cast<uint16_t>(port));
        v6.sin6_addr = in6addr_any;
        size = sizeof(sockaddr_in6);
    } else if (!socketAddressOf(address, port, local, size)) {
        error = "'" + address + "' is not an address";
        return noSocket;
    }
    RawSocket s = newRawSocket(local.ss_family, type);
    if (s == noSocket && both) {  // no IPv6 on this machine
        local = sockaddr_storage{};
        auto& v4 = reinterpret_cast<sockaddr_in&>(local);
        v4.sin_family = AF_INET;
        v4.sin_port = htons(static_cast<uint16_t>(port));
        v4.sin_addr.s_addr = htonl(INADDR_ANY);
        size = sizeof(sockaddr_in);
        s = newRawSocket(AF_INET, type);
    }
    if (s == noSocket) {
        error = netMessage(lastNetError());
        return noSocket;
    }
    if (local.ss_family == AF_INET6) {
        const int only = both ? 0 : 1;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only), sizeof(only));
    }
#ifndef _WIN32
    if (type == SOCK_STREAM) {
        const int reuse = 1;  // a port just let go of may be taken again at once, as on Windows
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }
#endif
    if (bind(s, reinterpret_cast<sockaddr*>(&local), size) != 0) {
        error = netMessage(lastNetError());
        closeRaw(s);
        return noSocket;
    }
    return s;
}

// A socket that takes TCP connections at `address` (none for every address, IPv4 and IPv6) and
// `port` (0 for one the system picks).
std::shared_ptr<Stream> listenOn(VM& vm, int64_t port, const std::string& address, int backlog) {
    std::string error;
    const RawSocket s = boundSocket(address, port, SOCK_STREAM, error);
    if (s == noSocket) {
        if (error.find("is not an address") != std::string::npos) throw ioError(vm, "'" + address + "' is not an address to listen on");
        throw ioError(vm, "cannot listen on port " + std::to_string(port) + ": " + error);
    }
    if (::listen(s, backlog) != 0) {
        const std::string why = netMessage(lastNetError());
        closeRaw(s);
        throw ioError(vm, "cannot listen on port " + std::to_string(port) + ": " + why);
    }
    auto listener = std::make_shared<Stream>(Stream::Kind::Listener);
    rawOf(*listener) = s;
    return listener;
}

// The address this end of a socket is at, as text; empty where it cannot say.
std::string localAddressText(Stream& s) {
    if (!s.unixPath.empty()) return std::string();
    sockaddr_storage address{};
    AddressSize size = sizeof(address);
    if (getsockname(rawOf(s), reinterpret_cast<sockaddr*>(&address), &size) != 0) return std::string();
    return addressText(address);
}

// The addresses `host` is found at by the system's resolver, each once, in the order it gives.
void lookUp(const std::string& host, Outcome& out) {
#ifdef _WIN32
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfoW* found = nullptr;
    const int failed = GetAddrInfoW(widen(host).c_str(), nullptr, &hints, &found);
#else
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    const int failed = ::getaddrinfo(host.c_str(), nullptr, &hints, &found);
#endif
    if (failed != 0 || found == nullptr) {
        out.unknownHost = true;
#ifdef _WIN32
        const bool missing = failed == WSAHOST_NOT_FOUND || failed == WSANO_DATA || found == nullptr;
        out.error = "cannot find host '" + host + "'" + (missing ? std::string() : ": " + netMessage(failed));
#else
        const bool missing = failed == EAI_NONAME || found == nullptr
#ifdef EAI_NODATA
                             || failed == EAI_NODATA
#endif
            ;
        out.error = "cannot find host '" + host + "'" + (missing ? std::string() : std::string(": ") + ::gai_strerror(failed));
#endif
        if (found != nullptr) {
#ifdef _WIN32
            FreeAddrInfoW(found);
#else
            ::freeaddrinfo(found);
#endif
        }
        return;
    }
    for (auto* entry = found; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6) continue;
        sockaddr_storage address{};
        std::memcpy(&address, entry->ai_addr, std::min<size_t>(entry->ai_addrlen, sizeof(address)));
        const std::string text = addressText(address);
        if (std::find(out.texts.begin(), out.texts.end(), text) == out.texts.end()) out.texts.push_back(text);
    }
#ifdef _WIN32
    FreeAddrInfoW(found);
#else
    ::freeaddrinfo(found);
#endif
}

// The name the system's resolver gives the address in `text`; empty where it has none.
std::string nameOf(const std::string& text) {
    sockaddr_storage address{};
    AddressSize size = 0;
    if (!socketAddressOf(text, 0, address, size)) return std::string();
#ifdef _WIN32
    wchar_t name[NI_MAXHOST] = {};
    if (GetNameInfoW(reinterpret_cast<const sockaddr*>(&address), size, name, NI_MAXHOST, nullptr, 0, NI_NAMEREQD) != 0) return std::string();
    return narrow(name, static_cast<int>(wcslen(name)));
#else
    char name[NI_MAXHOST] = {};
    if (::getnameinfo(reinterpret_cast<const sockaddr*>(&address), size, name, NI_MAXHOST, nullptr, 0, NI_NAMEREQD) != 0) return std::string();
    return name;
#endif
}

// A UDP socket at `address` and `port`, as boundSocket makes it.
std::shared_ptr<Stream> openDatagram(VM& vm, int64_t port, const std::string& address) {
    std::string error;
    const RawSocket s = boundSocket(address, port, SOCK_DGRAM, error);
    if (s == noSocket) {
        if (error.find("is not an address") != std::string::npos) throw ioError(vm, "'" + address + "' is not an address for a datagram socket");
        throw ioError(vm, "cannot open a datagram socket at port " + std::to_string(port) + ": " + error);
    }
#ifdef _WIN32
    // Windows would fail the next receive where a datagram sent earlier found no one listening
    // (an ICMP "port unreachable"); Linux leaves that unsaid, and so does this.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#endif
    auto made = std::make_shared<Stream>(Stream::Kind::Datagram);
    rawOf(*made) = s;
    sockaddr_storage local{};
    AddressSize size = sizeof(local);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &size) == 0) made->family = local.ss_family;
    return made;
}

// Sends `size` bytes from `data` as one datagram to `address` and `port`; false, with `error`
// filled in, where it cannot.
bool sendDatagram(Stream& s, const uint8_t* data, size_t size, const std::string& address, int64_t port, std::string& error) {
    Use use(s);
    if (s.closed.load()) {
        error = "the socket is closed";
        return false;
    }
    sockaddr_storage to{};
    AddressSize toSize = 0;
    if (!socketAddressOf(address, port, to, toSize)) {
        error = "'" + address + "' is not an address";
        return false;
    }
    if (s.family == AF_INET6) mapToIpv6(to, toSize);
    else if (s.family == AF_INET && to.ss_family == AF_INET6) {
        error = "a datagram socket at an IPv4 address cannot send to an IPv6 one (" + address + ")";
        return false;
    }
    const auto sent = sendto(rawOf(s), reinterpret_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&to), toSize);
    if (sent < 0) {
        const int code = lastNetError();
        error = s.closed.load() ? "the socket is closed" : "cannot send to " + address + ": " + netMessage(code);
        return false;
    }
    return true;
}

// Waits for one datagram, and gives up to `max` of its bytes (what does not fit is lost, as
// Java's receive loses it), where it came from, and its port.
void receiveDatagram(Stream& s, size_t max, Outcome& out) {
    Use use(s);
    if (s.closed.load()) {
        out.error = "the socket is closed";
        return;
    }
#ifndef _WIN32
    if (!waitReadable(s, s.readTimeout > 0 ? s.readTimeout : -1, out)) return;
#endif
    out.data.resize(std::max<size_t>(max, 1));
    sockaddr_storage from{};
    AddressSize fromSize = sizeof(from);
    for (;;) {
        auto got = recvfrom(rawOf(s), out.data.data(), static_cast<int>(out.data.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
        if (got < 0) {
            const int code = lastNetError();
#ifdef _WIN32
            if (code == WSAEMSGSIZE) got = static_cast<int>(out.data.size());  // cut to fit
#else
            if (code == EINTR) continue;
#endif
        }
        if (got < 0) {
            const int code = lastNetError();
            if (s.closed.load()) out.error = "the socket is closed";
            else if (code == timedOutError
#ifndef _WIN32
                     || code == EAGAIN || code == EWOULDBLOCK
#endif
            ) {
                out.error = "the read timed out";
                out.timedOut = true;
            } else out.error = "cannot receive a datagram: " + netMessage(code);
            out.data.clear();
            return;
        }
        out.data.resize(static_cast<size_t>(got) > max ? max : static_cast<size_t>(got));
        int port = 0;
        out.address = addressText(from, &port);
        out.number = port;
        return;
    }
}

std::vector<std::string> selfCommand;

}  // namespace

void setSelfCommand(std::vector<std::string> command) { selfCommand = std::move(command); }

// What a sandbox has no form of yet (M6.6.2): refused there, never done on the system.
void refuseInSandbox(VM& vm, const std::string& what) {
    if (vm.currentHost().isSandbox()) throw vm.errorOf("IOException", what + " is not available in a sandbox yet");
}

void registerIo(VM& vm) {
#ifndef _WIN32
    // A write to a pipe or a socket whose other end has gone is an IOException, not the end of
    // the program.
    ::signal(SIGPIPE, SIG_IGN);
#endif
    ObjClass* io = vm.alloc<ObjClass>(vm.intern("io"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        io->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("io.") + name, minArgs, maxArgs, fn)));
    };

    // ---- Streams ----

    // open(path, mode): "r" to read, "w" to write a new file, "a" to add to the end of one.
    def("open", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string path = vm.toStr(args[0], "file path");
        const std::string mode = vm.toStr(args[1], "mode");
        auto out = std::make_shared<Outcome>();
        std::shared_ptr<Host> host = vm.currentHostShared();
        return vm.blocking([=] {
                               if (!host->isSandbox()) {
                                   openFile(path, mode, *out);
                                   return;
                               }
                               // A sandbox's file (M6.6.2), through its channel.
                               std::shared_ptr<Channel> channel = host->openFile(path, mode, out->error);
                               if (channel == nullptr) return;
                               out->made = std::make_shared<Stream>(Stream::Kind::File);
                               out->made->channel = std::move(channel);
                           },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::integer(keep(out->made));
                           });
    });

    // ---- A file at a position (RandomAccessFile, M9/M10) ----

    // openRandom(path, mode): "r" to read, "rw" to read and write, the file made if it is not
    // there. Not in a sandbox yet: its files are streams.
    def("openRandom", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string path = vm.toStr(args[0], "file path");
        const std::string mode = vm.toStr(args[1], "mode");
        if (mode != "r" && mode != "rw")
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a RandomAccessFile is opened \"r\" to read or \"rw\" to read and write, not \"" + mode + "\"");
        refuseInSandbox(vm, "a RandomAccessFile");
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] { openRandom(path, mode == "rw", *out); },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::integer(keep(out->made));
                           });
    });

    // readAt(handle, position, count): up to `count` bytes from `position`, fewer at the end,
    // none past it.
    def("readAt", 3, 3, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const int64_t position = vm.toInt(args[1], "position");
        const int64_t count = vm.toInt(args[2], "count");
        if (position < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a position in a file is 0 or more, not " + std::to_string(position));
        if (count < 0 || count > (int64_t{1} << 31)) throw vm.runtimeError(Thrown::IllegalArgumentException, "readAt() reads 0 to 2^31 bytes, not " + std::to_string(count));
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                out->data.resize(static_cast<size_t>(count));
                const int64_t got = readAtPosition(*s, position, out->data.data(), out->data.size(), out->error);
                out->data.resize(got < 0 ? 0 : static_cast<size_t>(got));
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return bytesValue(vm, out->data);
            });
    });

    // readInto(handle, position, bytes, offset, count): read into `bytes` from `offset`, as
    // many as there are up to `count`; how many. Read off the program's thread, then copied in on
    // it, so that the bytes are only ever touched where the program runs.
    def("readInto", 5, 5, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const int64_t position = vm.toInt(args[1], "position");
        if (!args[2].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::ClassCastException, "readAt() reads into Bytes, not " + vm.typeName(args[2]));
        const int64_t offset = vm.toInt(args[3], "offset");
        const int64_t count = vm.toInt(args[4], "count");
        const int64_t size = static_cast<int64_t>(asBytes(args[2])->data.size());
        if (position < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a position in a file is 0 or more, not " + std::to_string(position));
        if (offset < 0 || count < 0 || offset > size || count > size - offset)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException, std::to_string(count) + " bytes from " + std::to_string(offset) + " do not fit in bytes of size " + std::to_string(size));
        auto out = std::make_shared<ScratchRead>();
        const Value into = args[2];
        return vm.blocking(
            [=] {
                out->buffer = scratchBuffers().take(static_cast<size_t>(count));
                const int64_t got = readAtPosition(*s, position, reinterpret_cast<char*>(out->buffer.data()), static_cast<size_t>(count), out->error);
                out->got = got < 0 ? 0 : got;
            },
            [out, into, offset](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                ObjBytes* bytes = asBytes(into);
                vm.guardWrite(bytes);
                if (offset + out->got > static_cast<int64_t>(bytes->data.size()))
                    throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "the bytes read into became smaller while the file was read");
                std::memcpy(bytes->data.data() + offset, out->buffer.data(), static_cast<size_t>(out->got));
                return Value::integer(out->got);
            });
    });

    // writeAt(handle, position, bytes, offset, count): `count` of the bytes from `offset`,
    // written at `position`; the file grows where they go past its end.
    def("writeAt", 5, 5, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const int64_t position = vm.toInt(args[1], "position");
        if (!args[2].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::ClassCastException, "writeAt() writes Bytes, not " + vm.typeName(args[2]));
        const int64_t offset = vm.toInt(args[3], "offset");
        const int64_t count = vm.toInt(args[4], "count");
        const std::vector<uint8_t>& data = asBytes(args[2])->data;
        const int64_t size = static_cast<int64_t>(data.size());
        if (position < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a position in a file is 0 or more, not " + std::to_string(position));
        if (offset < 0 || count < 0 || offset > size || count > size - offset)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException, std::to_string(count) + " bytes from " + std::to_string(offset) + " are not in bytes of size " + std::to_string(size));
        auto out = std::make_shared<Outcome>();
        // A copy, taken here: the program may change the bytes while the write is made.
        out->data.assign(reinterpret_cast<const char*>(data.data()) + offset, static_cast<size_t>(count));
        return vm.blocking([=] { writeAtPosition(*s, position, out->data.data(), out->data.size(), out->error); },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::null();
                           });
    });

    // fileSize(handle): the file's size now.
    def("fileSize", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] { out->number = sizeOfOpen(*s, out->error); },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::integer(out->number);
                           });
    });

    // resize(handle, size): the file made `size` bytes long, cut or grown with zeros.
    def("resize", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const int64_t size = vm.toInt(args[1], "size");
        if (size < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a file's size is 0 or more, not " + std::to_string(size));
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] { resizeOpen(*s, size, out->error); },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::null();
                           });
    });

    // sync(handle): what was written, on the disk before it returns.
    def("sync", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] { syncOpen(*s, out->error); },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::null();
                           });
    });

    // Up to `max` bytes, at least one; null at the end.
    def("read", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const int64_t wanted = vm.toInt(args[1], "number of bytes");
        if (wanted < 1) throw vm.runtimeError(Thrown::IllegalArgumentException, "read() takes how many bytes to read at most, which is at least 1");
        const size_t max = static_cast<size_t>(std::min<int64_t>(wanted, 1 << 30));
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                if (s->position == s->buffer.size() && !fill(*s, *out)) {
                    out->end = out->error.empty();
                    return;
                }
                const size_t count = std::min(max, s->buffer.size() - s->position);
                out->data.assign(s->buffer, s->position, count);
                s->position += count;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return out->end ? Value::null() : bytesValue(vm, out->data);
            });
    });

    // Everything up to the end, as bytes.
    def("readAll", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                while (fill(*s, *out)) {}
                if (!out->error.empty()) return;
                out->data.assign(s->buffer, s->position, std::string::npos);
                s->buffer.clear();
                s->position = 0;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return bytesValue(vm, out->data);
            });
    });

    // Everything up to the end, as text in the character set (D127).
    def("readText", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const Charset cs = charsetOf(vm, argc > 1 ? args[1] : Value::null());
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                Charset order = cs;
                if (!readingOrder(*s, order, *out)) return;
                while (fill(*s, *out)) {}
                if (!out->error.empty()) return;
                out->data = decodeText(std::string_view(s->buffer).substr(s->position), order);
                s->buffer.clear();
                s->position = 0;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return vm.str(std::move(out->data));
            });
    });

    // The next line, without its line end, in the character set (D127); null at the end.
    def("readLine", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const Charset cs = charsetOf(vm, argc > 1 ? args[1] : Value::null());
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                Charset order = cs;
                if (!readingOrder(*s, order, *out)) return;
                size_t searched = 0;  // how much of what is waiting has been looked through
                const size_t unit = isUtf16(order) ? 2 : 1;
                for (;;) {
                    size_t endSize = 1;
                    const size_t at = lineEnd(s->buffer, s->position + searched, order, endSize);
                    if (at != std::string::npos) {
                        out->data.assign(s->buffer, s->position, at - s->position);
                        s->position = at + endSize;
                        break;
                    }
                    searched = (s->buffer.size() - s->position) / unit * unit;
                    if (!fill(*s, *out)) {
                        if (!out->error.empty()) return;
                        // The end: what is left is the last line, if anything is.
                        if (s->position == s->buffer.size()) {
                            out->end = true;
                            return;
                        }
                        out->data.assign(s->buffer, s->position, std::string::npos);
                        s->position = s->buffer.size();
                        break;
                    }
                }
                out->data = decodeText(withoutCarriageReturn(std::move(out->data), order), order);
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return out->end ? Value::null() : vm.str(out->data);
            });
    });

    // Text (in the character set, D127) or bytes. Returns when all of it has been handed to
    // the system.
    def("write", 2, 3, [](VM& vm, int argc, Value* args) -> Value {
        // The program's own output or errors (Program.output, Program.errors): written here, in
        // turn with what print and printError write, to the same place.
        if (args[0].isInt() && (args[0].as.i == kStandardOutput || args[0].as.i == kStandardError)) {
            std::string data;
            if (args[1].isObjType(ObjType::Bytes)) data.assign(asBytes(args[1])->data.begin(), asBytes(args[1])->data.end());
            else data = encodeText(vm.stringify(args[1]), charsetOf(vm, argc > 2 ? args[2] : Value::null()));
            if (args[0].as.i == kStandardError) {
                vm.output->flush();  // what print wrote before goes first
                vm.errorOutput->write(data.data(), static_cast<std::streamsize>(data.size()));
                vm.errorOutput->flush();
            } else {
                vm.output->write(data.data(), static_cast<std::streamsize>(data.size()));
            }
            return Value::null();
        }
        auto s = streamOf(vm, args[0]);
        std::string data;
        // In Charset.utf16, a byte order mark before the stream's first text, none after (G53).
        bool marked = false;
        if (args[1].isObjType(ObjType::Bytes)) {
            data.assign(asBytes(args[1])->data.begin(), asBytes(args[1])->data.end());
        } else {
            const Charset cs = charsetOf(vm, argc > 2 ? args[2] : Value::null());
            marked = cs == Charset::Utf16;
            data = encodeText(vm.stringify(args[1]), marked ? Charset::Utf16Be : cs);
        }
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [s, out, marked, data = std::move(data)] {
                std::lock_guard<std::mutex> hold(s->writeMutex);
                if (marked && !s->markWritten) {
                    s->markWritten = true;
                    if (!rawWrite(*s, "\xFE\xFF", 2, out->error)) return;
                }
                rawWrite(*s, data.data(), data.size(), out->error);
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return Value::null();
            });
    });

    def("close", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<Stream> s = findStream(args[0], true);
        if (s == nullptr) return Value::null();
        // Closing may have to wait for a thread that is reading it to notice.
        return vm.blocking([s] { s->close(); }, [](VM&) -> Value { return Value::null(); });
    });

    def("isOpen", 1, 1, [](VM&, int, Value* args) -> Value {
        if (args[0].isInt() && (args[0].as.i == kStandardOutput || args[0].as.i == kStandardError)) return Value::boolean(true);
        const std::shared_ptr<Stream> s = findStream(args[0]);
        return Value::boolean(s != nullptr && !s->closed.load());
    });

    // standard(which): the handle of the program's own output (1) or errors (2), for a Writer.
    // It is never closed: closing it does nothing, as it is the program's, not the writer's.
    def("standard", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const int64_t which = vm.toInt(args[0], "which");
        if (which != 1 && which != 2) throw vm.runtimeError(Thrown::IllegalArgumentException, "standard(1) is the output, standard(2) the errors");
        return Value::integer(which == 1 ? kStandardOutput : kStandardError);
    });

    // ---- Processes ----

    // spawn(command, directory, input, output, error, environment, limits): starts a program. Gives
    // [process, its input, its output, its error output], the last three null unless "pipe".
    // `limits`, if given, is [bytes of memory, processes at once] for it and all it starts
    // together, each null for none (2026-11-20).
    def("spawn", 6, 7, [](VM& vm, int argc, Value* args) -> Value {
        refuseInSandbox(vm, "starting a program");
        if (!args[0].isObjType(ObjType::List) || asList(args[0])->size() == 0)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a process is started from a list: the program, then its arguments");
        Spawn spawn;
        for (Value part : vm.listCopy(asList(args[0]))) spawn.command.push_back(vm.stringify(part));
        if (!args[1].isNull()) spawn.directory = vm.toStr(args[1], "directory");
        spawn.modes[0] = vm.toStr(args[2], "input stream");
        spawn.modes[1] = vm.toStr(args[3], "output stream");
        spawn.modes[2] = vm.toStr(args[4], "error stream");
        spawn.environment = args[5];
        if (argc > 6 && !args[6].isNull()) {
            if (!args[6].isObjType(ObjType::List) || asList(args[6])->size() != 2)
                throw vm.runtimeError(Thrown::IllegalArgumentException, "a process's limits are [memory, processes]");
            const Value memory = vm.listGet(asList(args[6]), 0), processes = vm.listGet(asList(args[6]), 1);
            spawn.memory = memory.isNull() ? 0 : std::max<int64_t>(vm.toInt(memory, "memory"), 1);
            spawn.processes = processes.isNull() ? 0 : std::max<int64_t>(vm.toInt(processes, "processes"), 1);
        }
        const std::array<std::shared_ptr<Stream>, 4> made = startProcess(vm, spawn);

        ObjList* result = newList(vm);
        TempRoot keepResult(vm, Value::object(result));
        for (const std::shared_ptr<Stream>& stream : made) result->values().push_back(stream == nullptr ? Value::null() : Value::integer(keep(stream)));
        return Value::object(result);
    });

    // Waits for a process to end: its exit code, or null if `timeout` ms ran out first.
    def("processWait", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const int64_t limit = argc > 1 && !args[1].isNull() ? static_cast<int64_t>(std::clamp<double>(vm.toNumber(args[1], "time limit"), 0, 4e9)) : -1;
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                if (!waitProcess(*s, limit)) {
                    out->end = true;  // still running
                    return;
                }
                out->number = s->exitCode.load();
            },
            [out](VM&) -> Value { return out->end ? Value::null() : Value::integer(out->number); });
    });

    // collect(process, output, error, charset): reads what a process writes to its output and
    // its error output, each to its end, at the same time (so that neither fills up while the
    // other is read), and waits for it to end. Gives [exit code, output, error], the text in
    // the character set; either stream may be null, giving "".
    // collect(process, output, errors, charset, timeout?): what the program writes, read to its end,
    // and its exit code; with a timeout in milliseconds, the program is killed when it runs
    // longer, and a TimeoutException thrown (8.8h).
    def("collect", 4, 5, [](VM& vm, int argc, Value* args) -> Value {
        auto process = streamOf(vm, args[0]);
        std::shared_ptr<Stream> pipes[2] = {args[1].isNull() ? nullptr : streamOf(vm, args[1]), args[2].isNull() ? nullptr : streamOf(vm, args[2])};
        const Charset cs = charsetOf(vm, args[3]);
        const int64_t limit = argc > 4 && !args[4].isNull() ? std::max<int64_t>(vm.toInt(args[4], "timeout"), 0) : -1;
        auto out = std::make_shared<Outcome>();
        auto texts = std::make_shared<std::array<std::string, 2>>();
        auto killed = std::make_shared<std::atomic<bool>>(false);
        return vm.blocking(
            [=] {
                // A watch that kills the program when the time is up, which ends its pipes too.
                std::mutex watchMutex;
                std::condition_variable watchWake;
                bool finished = false;
                std::thread watch;
                if (limit >= 0 && process->simulated != nullptr)
                    // A sandbox's program: a time limit is up at once on one that waits for ever.
                    watch = std::thread([&, sim = process->simulated] {
                        int code = 0;
                        if (!waitSimulated(*sim, limit, code)) {
                            killed->store(true);
                            endSimulated(*sim, 1, true);
                        }
                    });
                else if (limit >= 0)
                    watch = std::thread([&] {
                        std::unique_lock<std::mutex> hold(watchMutex);
                        if (!watchWake.wait_for(hold, std::chrono::milliseconds(limit), [&] { return finished; }) && !process->exited.load()) {
                            killed->store(true);
                            killProcess(*process);
                        }
                    });
                std::string errors[2];
                auto drain = [&](int k) {
                    if (pipes[k] == nullptr) return;
                    Stream& s = *pipes[k];
                    std::lock_guard<std::mutex> hold(s.readMutex);
                    Outcome mine;
                    while (fill(s, mine)) {}
                    errors[k] = mine.error;
                    (*texts)[k] = decodeText(std::string_view(s.buffer).substr(s.position), cs);
                    s.buffer.clear();
                    s.position = 0;
                };
                std::thread second(drain, 1);
                drain(0);
                second.join();
                out->error = !errors[0].empty() ? errors[0] : errors[1];
                waitProcess(*process, -1);
                out->number = process->exitCode.load();
                if (watch.joinable()) {
                    {
                        std::lock_guard<std::mutex> hold(watchMutex);
                        finished = true;
                    }
                    watchWake.notify_all();
                    watch.join();
                }
            },
            [out, texts, killed, limit](VM& vm) -> Value {
                if (killed->load())
                    throw vm.errorOf("TimeoutException", "the program ran longer than " + std::to_string(limit) + " ms, and was stopped");
                if (!out->error.empty()) throw ioError(vm, *out);
                ObjList* result = newList(vm);
                TempRoot keepResult(vm, Value::object(result));
                result->values().push_back(Value::integer(out->number));
                for (std::string& text : *texts) result->values().push_back(vm.str(std::move(text)));
                return Value::object(result);
            });
    });

    def("processAlive", 1, 1, [](VM&, int, Value* args) -> Value {
        const std::shared_ptr<Stream> s = findStream(args[0]);
        if (s == nullptr || s->exited.load() || s->closed.load()) return Value::boolean(false);
        return Value::boolean(!waitProcess(*s, 0));
    });

    // Ends the program and every program it started that is still running.
    def("processKill", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        if (!s->exited.load() && !s->closed.load()) killProcess(*s);
        return Value::null();
    });

    // The limit a program started with limits went past, and was ended for: "memory",
    // "processes", or null.
    def("processPast", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = findStream(args[0]);
        if (s == nullptr || s->tree == nullptr) return Value::null();
        s->tree->settle();
        const int past = s->tree->past.load();
        return past == 1 ? vm.str("memory") : past == 2 ? vm.str("processes") : Value::null();
    });

    def("processId", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto s = streamOf(vm, args[0]);
        return Value::integer(s->simulated != nullptr ? s->simulated->pid : static_cast<int64_t>(s->pid));
    });

    // ---- A sandbox's programs (M6.6.2 step 4); only in one ----

    // A pipe: [the reading end, the writing end], each a stream.
    def("sandboxPipe", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (!vm.currentHost().isSandbox()) throw vm.runtimeError(Thrown::IllegalStateException, "a sandbox's pipe is made in a sandbox");
        auto buffer = std::make_shared<PipeBuffer>();
        auto reading = std::make_shared<Stream>(Stream::Kind::Pipe);
        reading->channel = std::make_shared<PipeEnd>(buffer, true);
        auto writing = std::make_shared<Stream>(Stream::Kind::Pipe);
        writing->channel = std::make_shared<PipeEnd>(buffer, false);
        return Value::object(newList(vm, {Value::integer(keep(reading)), Value::integer(keep(writing))}));
    });
    // A program of the sandbox's, as a Process stream: its end told by sandboxEnded.
    def("sandboxProcess", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (!vm.currentHost().isSandbox()) throw vm.runtimeError(Thrown::IllegalStateException, "a sandbox's program is started in a sandbox");
        static std::atomic<int64_t> nextPid{40000};
        auto made = std::make_shared<Stream>(Stream::Kind::Process);
        made->simulated = std::make_shared<SimulatedProcess>();
        made->simulated->pid = nextPid++;
        made->simulated->host = vm.currentHostShared();
        return Value::integer(keep(made));
    });
    // One of the program's own ends of its pipes, closed when it is killed.
    def("sandboxOwn", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const auto process = streamOf(vm, args[0]);
        const auto end = streamOf(vm, args[1]);
        if (process->simulated != nullptr && end->channel != nullptr) {
            std::lock_guard<std::mutex> hold(process->simulated->mutex);
            process->simulated->programEnds.push_back(end->channel);
        }
        return Value::null();
    });
    // The program has ended, with its exit code.
    def("sandboxEnded", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const auto process = streamOf(vm, args[0]);
        if (process->simulated != nullptr) endSimulated(*process->simulated, static_cast<int>(vm.toInt(args[1], "exit code")), false);
        return Value::null();
    });
    // The program waits for ever: until it is killed.
    def("sandboxHang", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto process = streamOf(vm, args[0]);
        auto sim = process->simulated;
        if (sim == nullptr) return Value::null();
        {
            std::lock_guard<std::mutex> hold(sim->mutex);
            sim->hanging = true;
            sim->changed.notify_all();
        }
        return vm.blocking([sim] {
            std::unique_lock<std::mutex> hold(sim->mutex);
            sim->changed.wait(hold, [&] { return sim->ended; });
        }, [](VM&) -> Value { return Value::null(); });
    });

    def("currentPid", 0, 0, [](VM&, int, Value*) -> Value { return Value::integer(currentProcessId()); });

    // What started this program: the path of its executable and, when ww is running a
    // script, the script. Starting that list again starts this program again.
    def("selfCommand", 0, 0, [](VM& vm, int, Value*) -> Value {
        ObjList* list = newList(vm);
        TempRoot keepList(vm, Value::object(list));
        for (const std::string& part : selfCommand) list->values().push_back(vm.str(part));
        return Value::object(list);
    });

    // ---- Sockets ----

    // connect(host, port, timeout): a TCP connection, tried for at most `timeout` ms if given.
    def("connect", 2, 3, [](VM& vm, int argc, Value* args) -> Value {
        refuseInSandbox(vm, "connecting to another computer");
        startNetworking(vm);
        const std::string host = vm.toStr(args[0], "host");
        const int64_t port = vm.toInt(args[1], "port");
        const int64_t timeout = argc > 2 && !args[2].isNull() ? std::max<int64_t>(vm.toInt(args[2], "time limit"), 0) : -1;
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] { connectTo(host, port, timeout, *out); },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::integer(keep(out->made));
                           });
    });

    // listen(port, address, backlog): a socket that takes connections. Port 0 lets the system
    // choose one; with no address it listens on every one this machine has.
    def("listen", 1, 3, [](VM& vm, int argc, Value* args) -> Value {
        refuseInSandbox(vm, "listening on a port");
        startNetworking(vm);
        const int64_t port = vm.toInt(args[0], "port");
        const std::string address = argc > 1 && !args[1].isNull() ? vm.toStr(args[1], "address") : std::string();
        const int backlog = argc > 2 && !args[2].isNull() ? static_cast<int>(vm.toInt(args[2], "backlog")) : 50;
        return Value::integer(keep(listenOn(vm, port, address, backlog)));
    });

    // Waits for a connection and gives it.
    def("accept", 1, 1, [](VM& vm, int, Value* args) -> Value {
        // Closed before this thread got here, it is gone from the table: said as a socket closed
        // while it waited is, whichever came first (io_streams.ww, under load).
        auto s = findStream(args[0]);
        if (s == nullptr) throw ioError(vm, "the socket is closed");
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] {
                               if (s->listener == nullptr) return acceptOn(*s, *out);
                               // A sandbox's port: the next connection made to it (step 5).
                               SandboxListener& l = *s->listener;
                               std::unique_lock<std::mutex> hold(l.mutex);
                               l.changed.wait(hold, [&] { return !l.waiting.empty() || l.closed; });
                               if (l.closed) {
                                   out->error = "the socket is closed";
                                   return;
                               }
                               out->made = l.waiting.front();
                               l.waiting.pop_front();
                           },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::integer(keep(out->made));
                           });
    });

    // connectUnix(path, timeout): a connection to a Unix domain socket's file, tried for at most
    // `timeout` ms if given (2026-11-19).
    def("connectUnix", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        refuseInSandbox(vm, "connecting to a Unix domain socket");
        startNetworking(vm);
        const std::string path = vm.toStr(args[0], "path");
        const int64_t timeout = argc > 1 && !args[1].isNull() ? std::max<int64_t>(vm.toInt(args[1], "time limit"), 0) : -1;
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] { connectUnix(path, timeout, *out); },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::integer(keep(out->made));
                           });
    });

    // listenUnix(path, backlog): a socket that takes connections at a Unix domain socket's file,
    // made now; closed, it removes the file.
    def("listenUnix", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        refuseInSandbox(vm, "listening on a Unix domain socket");
        startNetworking(vm);
        const std::string path = vm.toStr(args[0], "path");
        const int backlog = argc > 1 && !args[1].isNull() ? static_cast<int>(vm.toInt(args[1], "backlog")) : 50;
        return Value::integer(keep(listenUnix(vm, path, backlog)));
    });

    // The file of a Unix domain socket, or of one that listens; null for any other.
    def("socketPath", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto s = streamOf(vm, args[0]);
        return s->isSocket() && !s->unixPath.empty() ? vm.str(s->unixPath) : Value::null();
    });

    // [local port, remote address, remote port] of a socket; the last two null for one that listens.
    def("socketInfo", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        ObjList* list = newList(vm);
        TempRoot keepList(vm, Value::object(list));
        // A sandbox's socket or port (step 5).
        if (auto end = std::dynamic_pointer_cast<SocketEnd>(s->channel)) {
            list->values().push_back(Value::integer(end->localPort));
            list->values().push_back(vm.str(end->remoteHost));
            list->values().push_back(Value::integer(end->remotePort));
            return Value::object(list);
        }
        if (s->listener != nullptr) {
            list->values().push_back(Value::integer(s->listener->port));
            list->values().push_back(Value::null());
            list->values().push_back(Value::null());
            return Value::object(list);
        }
        int port = 0;
        list->values().push_back(localPort(*s, port) ? Value::integer(port) : Value::null());
        std::string host;
        if (s->kind == Stream::Kind::Socket && remoteEnd(*s, host, port)) {
            // An IPv4 address that a socket of both kinds took in (::ffff:1.2.3.4) is given as IPv4.
            if (host.rfind("::ffff:", 0) == 0 && host.find('.') != std::string::npos) host = host.substr(7);
            list->values().push_back(vm.str(host));
            list->values().push_back(Value::integer(port));
        } else {
            list->values().push_back(Value::null());
            list->values().push_back(Value::null());
        }
        const std::string local = localAddressText(*s);
        list->values().push_back(local.empty() ? Value::null() : vm.str(local));
        return Value::object(list);
    });

    // ---- Names, and datagrams (networking plan, part 1) ----

    // lookup(host): the addresses the system's resolver finds `host` at, as text, each once; an
    // UnknownHostException where it finds none.
    def("lookup", 1, 1, [](VM& vm, int, Value* args) -> Value {
        refuseInSandbox(vm, "looking up a name");
        startNetworking(vm);
        const std::string host = vm.toStr(args[0], "host");
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] { lookUp(host, *out); },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               ObjList* list = newList(vm);
                               TempRoot keepList(vm, Value::object(list));
                               for (const std::string& text : out->texts) list->values().push_back(vm.str(text));
                               return Value::object(list);
                           });
    });

    // reverseLookup(address): the name of the address in the text, or null where it has none.
    def("reverseLookup", 1, 1, [](VM& vm, int, Value* args) -> Value {
        refuseInSandbox(vm, "looking up a name");
        startNetworking(vm);
        const std::string address = vm.toStr(args[0], "address");
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] { out->data = nameOf(address); },
                           [out](VM& vm) -> Value { return out->data.empty() ? Value::null() : vm.str(out->data); });
    });

    // openDatagram(port, address): a UDP socket. Port 0 lets the system choose one; with no
    // address it takes datagrams at every address this machine has, IPv4 and IPv6.
    def("openDatagram", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        refuseInSandbox(vm, "a datagram socket");
        startNetworking(vm);
        const int64_t port = vm.toInt(args[0], "port");
        if (port < 0 || port > 65535) throw vm.runtimeError(Thrown::IllegalArgumentException, "a port is from 0 to 65535, not " + std::to_string(port));
        const std::string address = argc > 1 && !args[1].isNull() ? vm.toStr(args[1], "address") : std::string();
        return Value::integer(keep(openDatagram(vm, port, address)));
    });

    // sendDatagram(handle, bytes, offset, count, address, port): `count` of the bytes from
    // `offset`, sent as one datagram.
    def("sendDatagram", 6, 6, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        if (s->kind != Stream::Kind::Datagram) throw vm.runtimeError(Thrown::IllegalArgumentException, "not a datagram socket");
        if (!args[1].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::ClassCastException, "a datagram is Bytes, not " + vm.typeName(args[1]));
        const ObjBytes* bytes = asBytes(args[1]);
        const int64_t offset = vm.toInt(args[2], "offset");
        const int64_t count = vm.toInt(args[3], "count");
        const int64_t size = static_cast<int64_t>(bytes->data.size());
        if (offset < 0 || count < 0 || offset > size || count > size - offset)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException, std::to_string(count) + " bytes from " + std::to_string(offset) + " do not fit in bytes of size " + std::to_string(size));
        const std::string address = vm.toStr(args[4], "address");
        const int64_t port = vm.toInt(args[5], "port");
        if (port < 0 || port > 65535) throw vm.runtimeError(Thrown::IllegalArgumentException, "a port is from 0 to 65535, not " + std::to_string(port));
        // Sending does not wait (the system queues the datagram or drops it), so it runs here.
        std::string error;
        if (!sendDatagram(*s, bytes->data.data() + offset, static_cast<size_t>(count), address, port, error)) throw ioError(vm, error);
        return Value::null();
    });

    // receiveDatagram(handle, max): waits for a datagram; [its bytes (up to `max`), the address
    // it came from, its port]. A SocketTimeoutException past the socket's readTimeout.
    def("receiveDatagram", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto s = findStream(args[0]);
        if (s == nullptr) throw ioError(vm, "the socket is closed");
        if (s->kind != Stream::Kind::Datagram) throw vm.runtimeError(Thrown::IllegalArgumentException, "not a datagram socket");
        const int64_t max = vm.toInt(args[1], "size");
        if (max < 0 || max > 65535) throw vm.runtimeError(Thrown::IllegalArgumentException, "a datagram holds from 0 to 65535 bytes, not " + std::to_string(max));
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] { receiveDatagram(*s, static_cast<size_t>(max), *out); },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               const Value data = bytesValue(vm, out->data);
                               TempRoot keepData(vm, data);
                               const Value from = vm.str(out->address);
                               TempRoot keepFrom(vm, from);
                               return Value::object(newList(vm, {data, from, Value::integer(out->number)}));
                           });
    });

    // How long a read on a socket waits before giving up, in ms; 0 for as long as it takes.
    def("socketTimeout", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const auto s = streamOf(vm, args[0]);
        const int64_t limit = std::max<int64_t>(vm.toInt(args[1], "time limit"), 0);
        if (s->channel != nullptr) s->channel->setReadTimeout(limit);
        else setReadTimeout(*s, limit);
        return Value::null();
    });

    // No more will be written on this socket: the other side reads the end.
    def("shutdownOutput", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto s = streamOf(vm, args[0]);
        if (s->channel != nullptr) s->channel->shutdownOutput();
        else shutdownOutput(*s);
        return Value::null();
    });

    // ---- A sandbox's network (M6.6.2 step 5); only in one ----

    // A connection to `host`:`port` in a sandbox: [this program's end, the far side's end].
    def("sandboxSocketPair", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!vm.currentHost().isSandbox()) throw vm.runtimeError(Thrown::IllegalStateException, "a sandbox's connection is made in a sandbox");
        static std::atomic<int> nextPort{50000};
        auto link = std::make_shared<SocketLink>();
        link->host = vm.currentHostShared();
        const std::string host = vm.toStr(args[0], "host");
        const int port = static_cast<int>(vm.toInt(args[1], "port"));
        const int local = nextPort++;
        auto ours = std::make_shared<Stream>(Stream::Kind::Socket);
        ours->channel = std::make_shared<SocketEnd>(link, 0, host, port, local);
        auto theirs = std::make_shared<Stream>(Stream::Kind::Socket);
        theirs->channel = std::make_shared<SocketEnd>(link, 1, "10.0.0.2", local, port);
        return Value::object(newList(vm, {Value::integer(keep(ours)), Value::integer(keep(theirs))}));
    });
    // A port a program listens on in a sandbox: a Listener stream.
    def("sandboxListen", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (!vm.currentHost().isSandbox()) throw vm.runtimeError(Thrown::IllegalStateException, "a sandbox's port is made in a sandbox");
        auto made = std::make_shared<Stream>(Stream::Kind::Listener);
        made->listener = std::make_shared<SandboxListener>();
        made->listener->port = static_cast<int>(vm.toInt(args[0], "port"));
        return Value::integer(keep(made));
    });
    // A connection made to a port the program listens on: the end of what connects; the other is
    // the next accept()'s.
    def("sandboxDial", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto listening = streamOf(vm, args[0]);
        if (listening->listener == nullptr) throw vm.runtimeError(Thrown::IllegalArgumentException, "not a sandbox's port");
        SandboxListener& l = *listening->listener;
        auto link = std::make_shared<SocketLink>();
        link->host = vm.currentHostShared();
        static std::atomic<int> nextPort{60000};
        const int from = nextPort++;
        auto ours = std::make_shared<Stream>(Stream::Kind::Socket);
        ours->channel = std::make_shared<SocketEnd>(link, 0, "127.0.0.1", l.port, from);
        auto served = std::make_shared<Stream>(Stream::Kind::Socket);
        served->channel = std::make_shared<SocketEnd>(link, 1, "10.0.0.2", from, l.port);
        {
            std::lock_guard<std::mutex> hold(l.mutex);
            if (l.closed) throw vm.errorOf("IOException", "cannot connect to port " + std::to_string(l.port) + ": the connection was refused");
            l.waiting.push_back(served);
            l.changed.notify_all();
        }
        return Value::integer(keep(ours));
    });
    // The connection is reset: what either end does with it next fails.
    def("sandboxReset", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (auto end = std::dynamic_pointer_cast<SocketEnd>(streamOf(vm, args[0])->channel)) end->resetLink();
        return Value::null();
    });
    // This end waits for ever, until the connection is closed or reset: a read of the other's
    // with a time limit is up at once.
    def("sandboxSocketHang", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto end = std::dynamic_pointer_cast<SocketEnd>(streamOf(vm, args[0])->channel);
        if (end == nullptr) return Value::null();
        return vm.blocking([end] { end->hang(); }, [](VM&) -> Value { return Value::null(); });
    });

    vm.defineGlobal("__io", Value::object(io));
}

}  // namespace ww
