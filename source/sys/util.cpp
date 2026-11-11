#include "util.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sys {

bool readFile(const std::string& path, std::string& out, size_t maxBytes)
{
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[4096];
    bool ok = true;
    while (out.size() < maxBytes) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (n == 0)
            break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return ok;
}

std::string readLine(const std::string& path)
{
    std::string text;
    if (!readFile(path, text, 4096))
        return std::string();
    const size_t eol = text.find('\n');
    if (eol != std::string::npos)
        text.resize(eol);
    return trim(text);
}

bool readNumber(const std::string& path, long long& value, int base)
{
    std::string text;
    if (!readFile(path, text, 256) || text.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(text.c_str(), &end, base);
    if (end == text.c_str() || errno != 0)
        return false;
    value = v;
    return true;
}

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> splitWs(const std::string& s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])))
            ++j;
        if (j > i)
            out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t eol = s.find('\n', pos);
        if (eol == std::string::npos)
            eol = s.size();
        out.push_back(s.substr(pos, eol - pos));
        pos = eol + 1;
    }
    return out;
}

std::vector<std::string> listDir(const std::string& path)
{
    std::vector<std::string> out;
    DIR* dir = ::opendir(path.c_str());
    if (!dir)
        return out;
    while (const dirent* e = ::readdir(dir)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..")
            out.push_back(name);
    }
    ::closedir(dir);
    std::sort(out.begin(), out.end());
    return out;
}

std::string linkTarget(const std::string& path)
{
    char buf[PATH_MAX];
    const ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
    if (n <= 0)
        return std::string();
    return std::string(buf, static_cast<size_t>(n));
}

std::string realPath(const std::string& path)
{
    char buf[PATH_MAX];
    if (!::realpath(path.c_str(), buf))
        return std::string();
    return std::string(buf);
}

std::string baseName(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string dirName(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return std::string();
    return path.substr(0, slash);
}

bool pathExists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

}  // namespace sys
