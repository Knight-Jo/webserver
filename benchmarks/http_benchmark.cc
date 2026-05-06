#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct Options
{
    std::string host = "127.0.0.1";
    int port = 8080;
    int threads = 4;
    int connections = 4;
    int seconds = 10;
    std::string path = "/";
    std::string method = "GET";
    std::string body;
};

static void usage(const char *prog)
{
    std::cout << "Usage: " << prog
              << " <host> <port> <threads> <connections> <seconds> <path> [method] [body]\n";
}

static int connectToHost(const std::string &host, int port)
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0)
    {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next)
    {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0)
        {
            break;
        }
        ::close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

static bool sendAll(int fd, const std::string &data)
{
    const char *buf = data.data();
    size_t total = 0;
    while (total < data.size())
    {
        ssize_t n = ::send(fd, buf + total, data.size() - total, 0);
        if (n <= 0)
        {
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

static bool readN(int fd, std::string *buffer, size_t n)
{
    while (buffer->size() < n)
    {
        char temp[4096];
        ssize_t r = ::recv(fd, temp, sizeof(temp), 0);
        if (r <= 0)
        {
            return false;
        }
        buffer->append(temp, static_cast<size_t>(r));
    }
    return true;
}

static size_t findHeaderEnd(const std::string &buffer)
{
    std::string::size_type pos = buffer.find("\r\n\r\n");
    if (pos == std::string::npos)
    {
        return std::string::npos;
    }
    return pos + 4;
}

static size_t parseContentLength(const std::string &headers)
{
    std::string lower = headers;
    for (char &c : lower)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    std::string key = "content-length:";
    std::string::size_type pos = lower.find(key);
    if (pos == std::string::npos)
    {
        return 0;
    }
    pos += key.size();
    while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '\t'))
    {
        ++pos;
    }
    std::string::size_type end = pos;
    while (end < lower.size() && lower[end] >= '0' && lower[end] <= '9')
    {
        ++end;
    }
    if (end == pos)
    {
        return 0;
    }
    return static_cast<size_t>(std::strtoul(lower.substr(pos, end - pos).c_str(), nullptr, 10));
}

static bool readResponse(int fd)
{
    std::string buffer;
    while (true)
    {
        size_t headerEnd = findHeaderEnd(buffer);
        if (headerEnd != std::string::npos)
        {
            std::string headers = buffer.substr(0, headerEnd);
            size_t contentLength = parseContentLength(headers);
            size_t totalNeeded = headerEnd + contentLength;
            return readN(fd, &buffer, totalNeeded);
        }
        char temp[4096];
        ssize_t r = ::recv(fd, temp, sizeof(temp), 0);
        if (r <= 0)
        {
            return false;
        }
        buffer.append(temp, static_cast<size_t>(r));
    }
}

static std::string buildRequest(const Options &opt)
{
    std::ostringstream out;
    out << opt.method << " " << opt.path << " HTTP/1.1\r\n";
    out << "Host: " << opt.host << "\r\n";
    out << "Connection: keep-alive\r\n";
    if (opt.method == "POST")
    {
        out << "Content-Length: " << opt.body.size() << "\r\n";
        out << "Content-Type: text/plain\r\n";
    }
    out << "\r\n";
    if (opt.method == "POST")
    {
        out << opt.body;
    }
    return out.str();
}

static void worker(const Options &opt,
                   const std::string &request,
                   std::atomic<long long> *requests,
                   std::atomic<long long> *errors)
{
    std::vector<int> fds;
    fds.reserve(static_cast<size_t>(opt.connections));

    for (int i = 0; i < opt.connections; ++i)
    {
        int fd = connectToHost(opt.host, opt.port);
        if (fd < 0)
        {
            errors->fetch_add(1);
            continue;
        }
        fds.push_back(fd);
    }

    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::seconds(opt.seconds);

    while (std::chrono::steady_clock::now() < end)
    {
        for (size_t i = 0; i < fds.size(); ++i)
        {
            int fd = fds[i];
            if (!sendAll(fd, request))
            {
                ::close(fd);
                fds[i] = connectToHost(opt.host, opt.port);
                errors->fetch_add(1);
                continue;
            }
            if (!readResponse(fd))
            {
                ::close(fd);
                fds[i] = connectToHost(opt.host, opt.port);
                errors->fetch_add(1);
                continue;
            }
            requests->fetch_add(1);
        }
    }

    for (int fd : fds)
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
}

int main(int argc, char **argv)
{
    Options opt;
    if (argc < 7)
    {
        usage(argv[0]);
        return 1;
    }

    opt.host = argv[1];
    opt.port = std::atoi(argv[2]);
    opt.threads = std::atoi(argv[3]);
    opt.connections = std::atoi(argv[4]);
    opt.seconds = std::atoi(argv[5]);
    opt.path = argv[6];
    if (argc >= 8)
    {
        opt.method = argv[7];
    }
    if (argc >= 9)
    {
        opt.body = argv[8];
    }
    if (opt.threads <= 0 || opt.connections <= 0 || opt.seconds <= 0)
    {
        std::cerr << "Invalid options" << std::endl;
        return 1;
    }

    std::string request = buildRequest(opt);
    std::atomic<long long> requests(0);
    std::atomic<long long> errors(0);

    auto benchStart = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    for (int i = 0; i < opt.threads; ++i)
    {
        workers.emplace_back(worker, opt, request, &requests, &errors);
    }
    for (auto &t : workers)
    {
        t.join();
    }

    auto benchEnd = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(benchEnd - benchStart).count();
    double qps = requests.load() / elapsed;

    std::cout << "requests=" << requests.load()
              << " errors=" << errors.load()
              << " elapsed_s=" << elapsed
              << " qps=" << qps << std::endl;
    return 0;
}
