/*
 * Copyright (C) 2018  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of DualBootPatcher
 *
 * DualBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DualBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DualBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mbsystrace/procfs_p.h"

#include <memory>
#include <unordered_set>
#include <vector>

#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "mbcommon/error_code.h"
#include "mbcommon/integer.h"
#include "mbcommon/string.h"
#include "mbcommon/type_traits.h"


namespace mb::systrace::detail
{

using ScopedDIR = std::unique_ptr<DIR, TypeFn<closedir>>;
using ScopedFILE = std::unique_ptr<FILE, TypeFn<fclose>>;

static oc::result<void> ensure_procfs(int fd)
{
    if (fd < 0) {
        return std::errc::invalid_argument;
    } else if (struct stat sb; fstat(fd, &sb) != 0) {
        return ec_from_errno();
    } else if (major(sb.st_dev) != 0) {
        return std::errc::io_error;
    }

    return oc::success();
}

oc::result<pid_t> get_pid_status_field(pid_t pid, std::string_view name)
{
    char path[PATH_MAX];

    if (snprintf(path, sizeof(path), "/proc/%d/status", pid)
            >= static_cast<int>(sizeof(path))) {
        return std::errc::filename_too_long;
    }

    ScopedFILE fp(fopen(path, "re"));
    if (!fp) {
        return ec_from_errno();
    }

    // Ensure we're looking at procfs
    OUTCOME_TRYV(ensure_procfs(fileno(fp.get())));

    char buf[1024];

    while (fgets(buf, sizeof(buf), fp.get())) {
        std::string_view sv(buf);

        if (sv.size() > name.size() && starts_with(sv, name)
                && sv[name.size()] == ':') {
            sv.remove_prefix(name.size() + 1);
            if (!sv.empty() && sv.back() == '\n') {
                sv.remove_suffix(1);
            }

            pid_t result;
            if (!str_to_num(sv, 10, result)) {
                return ec_from_errno();
            }
            return result;
        }
    }

    if (ferror(fp.get())) {
        return ec_from_errno();
    } else {
        return std::errc::io_error;
    }
}

oc::result<pid_t> get_tgid(pid_t pid)
{
    return get_pid_status_field(pid, "Tgid");
}

oc::result<bool>
for_each_tid(pid_t pid, std::function<oc::result<bool>(pid_t)> func,
             bool retry_until_no_more)
{
    char path[PATH_MAX];

    if (snprintf(path, sizeof(path), "/proc/%d/task", pid)
            >= static_cast<int>(sizeof(path))) {
        return std::errc::filename_too_long;
    }

    ScopedDIR dp(opendir(path));
    if (!dp) {
        return ec_from_errno();
    }

    // Ensure we're looking at procfs
    OUTCOME_TRYV(ensure_procfs(dirfd(dp.get())));

    std::unordered_set<pid_t> tids;
    bool found_new_thread;

    // Same logic as gdb: Scan through a process's threads until no new threads
    // have been found for two iterations. This is only safe if "func" attaches
    // to the thread so it doesn't disappear. Otherwise, it's possible for a
    // thread to exit and a new thread to reuse the same TID.
    for (int it = 0; it < 2; ++it) {
        found_new_thread = false;

        dirent *ent;

        while ((ent = readdir(dp.get()))) {
            pid_t tid;

            if (strcmp(ent->d_name, ".") == 0
                    || strcmp(ent->d_name, "..") == 0) {
                continue;
            } else if (!str_to_num(ent->d_name, 10, tid)) {
                return ec_from_errno();
            }

            if (tids.find(tid) == tids.end()) {
                OUTCOME_TRY(add_to_list, func(tid));

                if (add_to_list) {
                    found_new_thread = true;
                    tids.insert(tid);
                }
            }
        }

        if (found_new_thread && retry_until_no_more) {
            rewinddir(dp.get());
            it = -1;
        }
    }

    return oc::success();
}

}
