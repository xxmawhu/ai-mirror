#include "ai_mirror/cli/commands.hpp"
#include <CLI/CLI.hpp>
#include <iostream>

namespace ai_mirror::cli {

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

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    if (create_cmd->parsed()) {
        return cmd_create(create_path, verbose);
    } else if (mkdir_cmd->parsed()) {
        return cmd_mkdir(mkdir_path, mkdir_user, verbose);
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
    }

    return 1;
}

}
