#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::core {

struct MountEntry {
    fs::path source;
    fs::path target;
    bool read_only;
    bool active;
};

class Graft {
public:
    Graft();

    bool bind_mount(const fs::path& source, const fs::path& target, bool read_only = true);
    bool unmount(const fs::path& target, bool lazy = false);
    bool unmount_all(const std::string& username);
    std::vector<MountEntry> list_mounts(const std::string& username) const;
    bool is_mounted(const fs::path& target) const;

    bool grant_write_access(const fs::path& path, const std::string& username);
    bool revoke_write_access(const fs::path& path, const std::string& username);
    bool ensure_group_exists(const std::string& groupname);
    bool set_directory_group(const fs::path& path, const std::string& groupname);
    bool set_sgid(const fs::path& path);

    std::vector<MountEntry> health_check() const;
    int force_cleanup(const std::vector<fs::path>& dead_mounts);

private:
    bool execute_mount(const fs::path& source, const fs::path& target, bool read_only);
    bool execute_umount(const fs::path& target, bool lazy);
    std::vector<MountEntry> parse_mount_table() const;
};

}
