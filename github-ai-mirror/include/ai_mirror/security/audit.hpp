#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::security {

struct AuditEntry {
    std::string category;
    std::string item;
    bool passed;
    std::string detail;
};

struct AuditReport {
    std::vector<AuditEntry> entries;
    int passed_count = 0;
    int failed_count = 0;

    void add(const std::string& category, const std::string& item,
             bool passed, const std::string& detail = "") {
        entries.push_back({category, item, passed, detail});
        if (passed) ++passed_count;
        else ++failed_count;
    }

    bool all_passed() const { return failed_count == 0; }
};

AuditReport audit_mounts_for_user(const std::string& username);
AuditReport audit_user_permissions(const std::string& username);
AuditReport audit_config_security(const fs::path& config_path);
AuditReport full_audit();

bool write_audit_report(const AuditReport& report, const fs::path& output_path);

}
