// Small file and string helpers shared by the backend.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sys {

// Reads a whole (pseudo-)file. Returns false if it cannot be opened or read.
bool readFile(const std::string& path, std::string& out, size_t maxBytes = 1 << 20);

// First line of a file, trimmed; empty if the file is missing.
std::string readLine(const std::string& path);

// Parses the leading integer of a file.
bool readNumber(const std::string& path, long long& value, int base = 10);

std::string trim(const std::string& s);
std::string toLower(std::string s);
std::vector<std::string> splitWs(const std::string& s);
std::vector<std::string> splitLines(const std::string& s);

// Directory entries without "." and "..", sorted.
std::vector<std::string> listDir(const std::string& path);

std::string linkTarget(const std::string& path);
std::string realPath(const std::string& path);
std::string baseName(const std::string& path);
std::string dirName(const std::string& path);
bool pathExists(const std::string& path);

inline bool startsWith(const std::string& s, const char* prefix)
{
    return s.rfind(prefix, 0) == 0;
}

}  // namespace sys
