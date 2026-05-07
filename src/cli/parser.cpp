#include "ai_mirror/cli/commands.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/core/config.hpp"
#include <CLI/CLI.hpp>
#include <iostream>
#include <pwd.h>

namespace ai_mirror::cli {

static bool is_ai_user() {
    auto config = core::ConfigParser::load_default();
    std::string prefix = config.user.prefix;
    std::string current = utils::get_effective_username();

    if (current.empty() || prefix.empty()) return false;
    if (current.length() <= prefix.length()) return false;
    if (current.substr(0, prefix.length()) != prefix) return false;

    size_t underscore_pos = current.find('_', prefix.length());
    return underscore_pos != std::string::npos;
}

int parse_and_run(int argc, char** argv) {
    CLI::App app{"ai-mirror — AI 时代的 Linux 用户隔离方案\n"
                 "为每个 AI 项目创建独立的 Linux 用户，使用原生权限体系实现沙箱隔离"};
    app.require_subcommand(1);

    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "显示详细输出");

    // create
    std::string create_path;
    auto* create_cmd = app.add_subcommand("create",
        "创建项目用户\n"
        "  为指定项目创建独立的 ai-user，完成全部初始化：\n"
        "  1. 创建 Linux 用户（用户名 = prefix + 主用户 + 项目名哈希）\n"
        "  2. 设置 SSH 免密登录（生成密钥对 + authorized_keys）\n"
        "  3. 只读 bind mount 主用户的配置文件（.bashrc, .config 等）\n"
        "  4. 授予项目目录读写权限\n"
        "  5. 输出创建的用户名");
    create_cmd->add_option("project_path", create_path, "项目目录路径")->required();

    // mkdir
    std::string mkdir_path;
    std::string mkdir_user;
    auto* mkdir_cmd = app.add_subcommand("mkdir",
        "授权目录\n"
        "  创建目录并授权 ai-user 读写");
    mkdir_cmd->add_option("path", mkdir_path, "目录路径")->required();
    mkdir_cmd->add_option("ai_user", mkdir_user, "AI 用户名")->required();

    // cp
    std::string cp_src, cp_dst;
    auto* cp_cmd = app.add_subcommand("cp",
        "复制文件\n"
        "  复制文件或目录，自动检测目标路径是否属于 ai-user 目录。\n"
        "  若属于 ai-user 目录，自动设置所有权；否则执行普通复制。\n"
        "  自动清除 SUID/SGID 位");
    cp_cmd->add_option("src", cp_src, "源路径")->required();
    cp_cmd->add_option("dst", cp_dst, "目标路径")->required();
    cp_cmd->add_flag("-f,--force", "强制覆盖已存在的目标文件");

    // mv
    std::string mv_src, mv_dst;
    auto* mv_cmd = app.add_subcommand("mv",
        "移动文件\n"
        "  移动文件或目录，自动检测目标路径是否属于 ai-user 目录。\n"
        "  同文件系统原子 rename，跨文件系统 copy+delete");
    mv_cmd->add_option("src", mv_src, "源路径")->required();
    mv_cmd->add_option("dst", mv_dst, "目标路径")->required();
    mv_cmd->add_flag("-f,--force", "强制覆盖已存在的目标文件");

    // touch
    std::string touch_path, touch_user;
    auto* touch_cmd = app.add_subcommand("touch",
        "创建文件并授权\n"
        "  创建空文件并设置 ai-user 所有权。\n"
        "  使用 O_NOFOLLOW + fchown 防符号链接攻击");
    touch_cmd->add_option("path", touch_path, "文件路径")->required();
    touch_cmd->add_option("ai_user", touch_user, "AI 用户名")->required();

    // cd
    std::string cd_path;
    auto* cd_cmd = app.add_subcommand("cd",
        "切换身份\n"
        "  根据目标路径所属用户，自动切换到对应的身份上下文：\n"
        "  - 普通目录：在当前 shell 中执行 cd\n"
        "  - ai-user 项目目录：通过 SSH 登录到 AI 用户\n"
        "  跨共享盘支持：ai-user 检测基于路径结构而非 UID");
    cd_cmd->add_option("path", cd_path, "目标路径")->required();

    // list
    app.add_subcommand("list",
        "列出用户\n"
        "  列出当前用户所拥有的 ai-user 及其 bind mount 状态\n"
        "  （仅显示属于调用者的 ai-user，其他用户的不可见）");

    // health
    app.add_subcommand("health",
        "健康检查\n"
        "  检查所有 bind mount 是否正常。\n"
        "  退出码 0 = 全部健康，1 = 存在异常。\n"
        "  可集成到 cron 定时任务");

    // force-destroy
    std::string destroy_target;
    auto* destroy_cmd = app.add_subcommand("force-destroy",
        "强制清理\n"
        "  强制卸载并删除 ai-user，不保留任何数据。\n"
        "  用于异常情况下的紧急清理。\n"
        "  输入识别：若输入符合有效用户名格式则直接使用；否则尝试从路径推导");
    destroy_cmd->add_option("target", destroy_target, "用户名或项目路径")->required();

    // rm
    std::string rm_path;
    auto* rm_cmd = app.add_subcommand("rm",
        "删除项目\n"
        "  安全删除项目的 ai-user，保留输出文件");
    rm_cmd->add_option("project_path", rm_path, "项目目录路径")->required();

    // config
    app.add_subcommand("config",
        "查看配置\n"
        "  显示当前加载的配置文件路径及所有配置项");

    // status
    app.add_subcommand("status",
        "项目状态\n"
        "  显示当前用户的 ai-user 详细信息：\n"
        "  home、UID、mount 状态、SSH 密钥、authorized_keys 健康状态\n"
        "  （仅显示属于调用者的 ai-user）");

    // update
    std::string update_path;
    auto* update_cmd = app.add_subcommand("update",
        "修复 SSH 和挂载\n"
        "  重新应用 SSH 密钥和 bind mount 配置，\n"
        "  用于修复异常状态（如密钥丢失、挂载失效等）");
    update_cmd->add_option("project_path", update_path, "项目目录路径")->required();

    // watch
    std::string watch_path, watch_user;
    auto* watch_cmd = app.add_subcommand("watch",
        "实时监控\n"
        "  以 htop 风格实时监控所有 ai-user 的状态\n"
        "  可选参数：指定项目路径和/或用户名进行过滤");
    watch_cmd->add_option("project_path", watch_path, "项目目录路径（可选，用于过滤）");
    watch_cmd->add_option("ai_user", watch_user, "AI 用户名（可选，用于过滤）");

    // init
    auto* init_cmd = app.add_subcommand("init",
        "初始化用户环境\n"
        "  确保当前用户环境正确配置 am 命令，包括：\n"
        "  1. 检查 ai-mirror 组成员身份\n"
        "  2. 创建用户配置文件 ~/.ai-mirror.toml\n"
        "  3. 在 ~/.bashrc 中添加 source am.sh（tmux 兼容）\n"
        "  4. 加载 bash 补齐\n"
        "  运行后新终端/tmux 窗口即可直接使用 am 命令");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // init can run without group check (it helps user set up membership)
    if (init_cmd->parsed()) {
        return cmd_init(verbose);
    }

    if (is_ai_user()) {
        std::cerr << "ai-mirror: AI users cannot use this tool." << std::endl;
        std::cerr << "This command manages AI user isolation and must be run by the main user." << std::endl;
        return 1;
    }

    if (!utils::is_group_member("ai-mirror")) {
        std::cerr << "ai-mirror: You must be a member of the 'ai-mirror' group to use this tool." << std::endl;
        std::cerr << "Please contact your administrator to be added to the group." << std::endl;
        return 1;
    }

    if (create_cmd->parsed()) {
        return cmd_create(create_path, verbose);
    } else if (mkdir_cmd->parsed()) {
        return cmd_mkdir(mkdir_path, mkdir_user, verbose);
    } else if (cp_cmd->parsed()) {
        return cmd_cp(cp_src, cp_dst, verbose);
    } else if (mv_cmd->parsed()) {
        return cmd_mv(mv_src, mv_dst, verbose);
    } else if (touch_cmd->parsed()) {
        return cmd_touch(touch_path, touch_user, verbose);
    } else if (cd_cmd->parsed()) {
        return cmd_cd(cd_path, verbose);
    } else if (app.got_subcommand("list")) {
        return cmd_list(verbose);
    } else if (app.got_subcommand("health")) {
        return cmd_health(verbose);
    } else if (destroy_cmd->parsed()) {
        return cmd_force_destroy(destroy_target, verbose);
    } else if (rm_cmd->parsed()) {
        return cmd_rm(rm_path, verbose);
    } else if (app.got_subcommand("config")) {
        return cmd_config(verbose);
    } else if (app.got_subcommand("status")) {
        return cmd_status(verbose);
    } else if (update_cmd->parsed()) {
        return cmd_update(update_path, verbose);
    } else if (watch_cmd->parsed()) {
        return cmd_watch(watch_path, watch_user, verbose);
    }

    return 1;
}

}
