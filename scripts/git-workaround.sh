#!/bin/bash
# git-workaround.sh — 绕过损坏的 /etc/gitconfig 和 BeeGFS 挂载的 $HOME/.gitconfig
# 
# 使用方法: source scripts/git-workaround.sh
# 或者: GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=<repo-config-path> git <cmd>
#
# 根因:
#   1. /etc/gitconfig 有 70+ 重复的 safe.directory 条目
#   2. $HOME/.gitconfig 是 BeeGFS 只读挂载（源文件已被删除）
#   3. Git 读取任一个失败都会报: "fatal: unknown error occurred while reading the configuration files"
#
# 修复方案:
#   - 短期: export GIT_CONFIG_NOSYSTEM=1 跳过系统级 config
#   - 长期: 需要 root 清理 /etc/gitconfig 重复条目 + 卸载 $HOME/.gitconfig 挂载
#
# 修复历史:
#   2026-06-30: install.sh 改为使用 'git config --replace-all safe.directory "*"'
#               避免重复追加。gitee/github/gitlib 子项目同步修复。
#
# 已验证: 2026-06-30

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_GLOBAL="${REPO_DIR}/.git/config"

echo "[git-workaround] GIT_CONFIG_NOSYSTEM=1"
echo "[git-workaround] GIT_CONFIG_GLOBAL=${GIT_CONFIG_GLOBAL}"
echo "[git-workaround] Now 'git' commands should work."
