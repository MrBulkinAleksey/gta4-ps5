/* The two helpers store_stfs.cpp takes from Recomp Store's store_zip (the rest of
 * that file, the zip reader, isn't needed here). Same code as the store's.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <string>
#include <sys/stat.h>

namespace recomp::zip
{
/* A name that can only land inside the folder: no absolute paths, no "..". */
inline bool safe_name(const std::string &n)
{
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find('\\') != std::string::npos || n.find(':') != std::string::npos)
        return false;
    std::size_t at = 0;
    while (at <= n.size())
    {
        std::size_t end = n.find('/', at);
        if (end == std::string::npos)
            end = n.size();
        if (n.compare(at, end - at, "..") == 0 && end - at == 2)
            return false;
        at = end + 1;
    }
    return true;
}

inline void make_dirs(const std::string &path)
{
    for (std::size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/')
        {
            const std::string d = path.substr(0, i);
            if (mkdir(d.c_str(), 0777) == 0)
                chmod(d.c_str(), 0777);
        }
}
} // namespace recomp::zip
