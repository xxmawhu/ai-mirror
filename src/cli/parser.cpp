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
    CLI::App app{"ai-mirror - Linux user permission isolation manager"};
    app.require_subcommand(1);

    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Show detailed output");

    // create
    std::string create_path;
    auto* create_cmd = app.add_subcommand("create", "Create an ai-user for a project");
    create_cmd->add_option("project_path", create_path, "Path to the project directory")->required();

    // mkdir
    std::string mkdir_path;
    std::string mkdir_user;
    auto* mkdir_cmd = app.add_subcommand("mkdir", "Grant write access to an ai-user");
    mkdir_cmd->add_option("path", mkdir_path, "Directory path")->required();
    mkdir_cmd->add_option("ai_user", mkdir_user, "AI user to grant access")->required();

    // cp
    std::string cp_src, cp_dst;
    auto* cp_cmd = app.add_subcommand("cp", "Copy file/directory, auto-detect ai-user ownership");
    cp_cmd->add_option("src", cp_src, "Source path")->required();
    cp_cmd->add_option("dst", cp_dst, "Destination path")->required();

    // mv
    std::string mv_src, mv_dst;
    auto* mv_cmd = app.add_subcommand("mv", "Move file/directory atomically, auto-detect ai-user ownership");
    mv_cmd->add_option("src", mv_src, "Source path")->required();
    mv_cmd->add_option("dst", mv_dst, "Destination path")->required();

    // touch
    std::string touch_path, touch_user;
    auto* touch_cmd = app.add_subcommand("touch", "Create file and grant ownership to ai-user");
    touch_cmd->add_option("path", touch_path, "File path to create")->required();
    touch_cmd->add_option("ai_user", touch_user, "AI user to grant ownership")->required();

    // cd
    std::string cd_path;
    auto* cd_cmd = app.add_subcommand("cd", "Switch to appropriate user context");
    cd_cmd->add_option("path", cd_path, "Target path")->required();

    // list
    app.add_subcommand("list", "List all managed ai-users");

    // health
    app.add_subcommand("health", "Check health of all mounts");

    // force-destroy
    std::string destroy_target;
    auto* destroy_cmd = app.add_subcommand("force-destroy", "Force remove an ai-user and its mounts");
    destroy_cmd->add_option("target", destroy_target, "Username or project path")->required();

    // rm
    std::string rm_path;
    auto* rm_cmd = app.add_subcommand("rm", "Remove project ai-user, preserving output files");
    rm_cmd->add_option("project_path", rm_path, "Path to the project directory")->required();

    // config
    app.add_subcommand("config", "Show current configuration");

    // status
    app.add_subcommand("status", "Show status of all projects");

    // update
    std::string update_path;
    auto* update_cmd = app.add_subcommand("update", "Re-apply SSH and mount fixes for a project");
    update_cmd->add_option("project_path", update_path, "Path to the project directory")->required();

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
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
    }

    return 1;
}

}
