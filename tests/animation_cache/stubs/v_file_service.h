#pragma once

// Only the platform IO boundary is replaced. Production animation loading,
// reference counting, frame stepping, bones, cache lookup and eviction run intact.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

class INIFILE
{
  public:
    bool ReadString(const char *, const char *key, char *out, size_t size, const char *)
    {
        if (std::strcmp(key, "animation") != 0)
            return false;
        std::snprintf(out, size, "%s", "fixture.an");
        return true;
    }
    bool ReadStringNext(const char *, const char *, char *, size_t) { return false; }
    bool GetSectionName(char *, size_t) { return false; }
    bool GetSectionNameNext(char *, size_t) { return false; }
    int GetInt(const char *, const char *, int fallback) { return fallback; }
    float GetFloat(const char *, const char *, float fallback) { return fallback; }
};

struct TestFiles
{
    std::string fixture;
    unsigned iniReads = 0;
    unsigned animationReads = 0;
    std::unique_ptr<INIFILE> OpenIniFile(const char *)
    {
        ++iniReads;
        return std::make_unique<INIFILE>();
    }
    std::fstream _CreateFile(const char *, std::ios::openmode mode)
    {
        ++animationReads;
        return std::fstream(fixture, mode);
    }
    bool _ReadFile(std::fstream &file, void *data, std::streamsize size)
    {
        return static_cast<bool>(file.read(static_cast<char *>(data), size));
    }
    void _CloseFile(std::fstream &file) { file.close(); }
};

extern TestFiles *fio;
