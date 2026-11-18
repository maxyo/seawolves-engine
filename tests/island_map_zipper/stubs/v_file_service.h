#pragma once

// The file service boundary of MapZipper (src/libs/island/src/map_zipper.cpp): game files are plain files here,
// with the production contract that a failed read or write returns false.
#include <fstream>
#include <string>

struct TestFiles
{
    std::string root;
    std::fstream _CreateFile(const char *name, std::ios::openmode mode)
    {
        return std::fstream(root + "/" + name, mode);
    }
    bool _ReadFile(std::fstream &file, void *data, std::streamsize size)
    {
        return static_cast<bool>(file.read(static_cast<char *>(data), size));
    }
    bool _WriteFile(std::fstream &file, const void *data, std::streamsize size)
    {
        return static_cast<bool>(file.write(static_cast<const char *>(data), size));
    }
    void _CloseFile(std::fstream &file)
    {
        if (file.is_open())
            file.close();
    }
};

extern TestFiles *fio;
