#!/usr/bin/env python3
"""
check-am-fix-amount.py — 扫描所有项目 .am_status，检查 source/target 路径

一次性自动执行：
  1. git pull + bash install.sh (ai-mirror release)
  2. 扫描 /mnt/beegfs_data/usr/maxx/dev/ 下所有 .am_status
  3. 检查每个 mount 条目的 source/target
  4. 发现异常 → 尝试 am auto-fix-all → 重新验证
  5. 仍有异常 → 向 aimirror 发布严厉 issue

用法:
  ./check-am-fix-amount.py

返回值:
  0 = 全部正常
  1 = 存在异常
"""

import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone

# === 硬编码配置 ===
RELEASE_DIR = os.path.expanduser("~maxx/release/ai-mirror")
DEV_SCAN_ROOT = "/mnt/beegfs_data/usr/maxx/dev"
WRONG_PREFIX = "/usr/maxx/"

# 已知虚拟/伪文件系统类型（与 ai-mirror vfs_util.hpp 同步）
# 这些文件系统的 stat 在 source 上可能返回零（ino=0, dev=0），
# 因为 /proc/mounts 的 device 列显示的是虚拟设备名而非真实路径。
# BeegFS 是典型的例子：device=beegfs_nodev, fstype=beegfs。
VIRTUAL_FSTYPES = frozenset({
    "proc", "tmpfs", "devtmpfs", "sysfs", "cgroup", "cgroup2", "devpts",
    "none", "binfmt_misc", "configfs", "debugfs", "tracefs", "securityfs",
    "pstore", "hugetlbfs", "mqueue", "fusectl", "efivarfs", "bpf", "autofs",
    "overlay", "aufs", "fuse.portal", "beegfs",
    # BeeGFS 内核模块返回的 fstype 实际是 "fhgfs"（原名 FhGFS）
    "fhgfs",
})


def get_main_user_home(username: str) -> str:
    try:
        import pwd
        return pwd.getpwnam(username).pw_dir
    except (ImportError, KeyError):
        pass
    try:
        with open("/etc/passwd", "r") as f:
            for line in f:
                parts = line.strip().split(":")
                if parts[0] == username:
                    return parts[5]
    except OSError:
        pass
    return ""


def read_status(project_path: str) -> dict | None:
    status_path = os.path.join(project_path, ".am_status")
    if not os.path.exists(status_path):
        return None
    try:
        with open(status_path, "r") as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        return None


def get_fstype_from_source(source: str) -> str:
    """通过 stat -f 获取文件所在文件系统的类型。

    用于 .am_status 中 fstype 为 '-' 或空的情况（旧版本遗留格式），
    直接查询内核以确定是否属于虚拟文件系统。
    """
    if not source or not os.path.exists(source):
        return ""
    try:
        result = subprocess.run(
            ["stat", "-f", "-c", "%T", source],
            capture_output=True, text=True, timeout=5,
        )
        if result.returncode == 0:
            return result.stdout.strip().lower()
    except (subprocess.SubprocessError, FileNotFoundError, OSError):
        pass
    return ""


def check_mount_entry(mount: dict, project_path: str, main_user: str, expected_home: str) -> list[str]:
    issues = []
    source = mount.get("source", "")
    target = mount.get("target", "")
    source_stat = mount.get("source_stat", {})

    if not target.startswith(project_path):
        issues.append(f"target 不在项目目录内: target={target}, project={project_path}")

    if not source.startswith("/"):
        return issues

    source_exists = os.path.exists(source)
    if not source_exists:
        issues.append(f"source 不存在: {source} → {target}")

    stat_all_zero = (
        source_stat.get("ino") == 0
        and source_stat.get("dev") == 0
        and source_stat.get("size") == 0
    )
    if stat_all_zero and source_exists:
        # 检查是否属于虚拟文件系统（如 BeeGFS）
        # 虚拟文件系统的 source_stat 全零是预期行为（ai-mirror 跳过 stat
        # 以避免分布式文件系统的性能开销）
        fstype = mount.get("fstype", "")
        if not fstype or fstype == "-":
            fstype = get_fstype_from_source(source)
        if fstype in VIRTUAL_FSTYPES:
            # 虚拟文件系统 — source_stat 全零是预期的，不报错
            pass
        else:
            issues.append(f"source 存在但 stat 异常: {source} (ino=0, dev=0)")
    elif stat_all_zero and not source_exists:
        issues.append(f"source 不存在且 stat 无效: {source}")

    if expected_home and source.startswith("/") and not source.startswith(expected_home):
        source_parts = source.lstrip("/").split("/")
        if main_user in source_parts:
            user_idx = len(source_parts) - 1 - source_parts[::-1].index(main_user)
            rel_parts = source_parts[user_idx + 1:]
            if rel_parts:
                rel_path = "/".join(rel_parts)
                expected_source = os.path.join(expected_home, rel_path)
                issues.append(
                    f"source 路径错误: 当前={source}, "
                    f"预期={expected_source} (主用户 home 为 {expected_home})"
                )
        else:
            for prefix in ["/usr/", "/home/"]:
                if source.startswith(prefix):
                    after_prefix = source[len(prefix):]
                    parts = after_prefix.split("/", 1)
                    if len(parts) == 2:
                        rel_path = parts[1]
                        expected_source = os.path.join(expected_home, rel_path)
                        issues.append(
                            f"source 路径错误: 当前={source}, "
                            f"预期={expected_source} (主用户 home 为 {expected_home})"
                        )
                        break
    return issues


def format_issues(mounts_issues: list[tuple[dict, list[str]]]) -> str:
    lines = []
    for mount, issues in mounts_issues:
        source = mount.get("source", "?")
        target = mount.get("target", "?")
        lines.append(f"## source: {source}")
        lines.append(f"   target: {target}")
        for issue in issues:
            lines.append(f"   ❌ {issue}")
        lines.append("")
    return "\n".join(lines)


def run_cmd(cmd: list[str], cwd: str | None = None,
            timeout: int = 120) -> subprocess.CompletedProcess | None:
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=timeout)
        return result
    except FileNotFoundError:
        print(f"[WARN] 命令未找到: {cmd[0]}", file=sys.stderr)
        return None
    except subprocess.TimeoutExpired:
        print(f"[WARN] 命令超时 ({timeout}s): {' '.join(cmd)}", file=sys.stderr)
        return None


def update_ai_mirror() -> list[str]:
    """三部曲: git pull → bash install.sh。返回错误列表。"""
    errors: list[str] = []

    if not os.path.isdir(RELEASE_DIR):
        err = f"release 目录不存在: {RELEASE_DIR}"
        print(f"[ERROR] {err}", file=sys.stderr)
        errors.append(err)
        return errors

    print(f"{'='*60}")
    print(f"ai-mirror 更新")
    print(f"  release: {RELEASE_DIR}")
    print(f"{'='*60}")

    # Step 1: git pull
    print(f"[1/2] git pull ...")
    result = run_cmd(["git", "pull"], cwd=RELEASE_DIR)
    if result is not None and result.returncode == 0:
        summary = result.stdout.strip().split("\n")[-1] if result.stdout.strip() else ""
        print(f"  ✓ {summary}")
    else:
        code = result.returncode if result else "N/A"
        print(f"  ⚠ git pull 失败 (code={code})")

    # Step 2: bash install.sh
    install_script = os.path.join(RELEASE_DIR, "install.sh")
    if os.path.exists(install_script):
        print(f"[2/2] bash install.sh ...")
        result = run_cmd(["bash", "install.sh"], cwd=RELEASE_DIR)
        if result is not None and result.returncode == 0:
            print(f"  ✓ install.sh 完成")
        else:
            code = result.returncode if result else "N/A"
            err = f"install.sh 失败 (code={code})"
            if result and result.stderr.strip():
                err += f" | {result.stderr.strip()[-200:]}"
            print(f"  ⚠ {err}")
            errors.append(err)
    else:
        print(f"[2/2] install.sh 不存在，跳过")

    print()
    return errors


def enumerate_ai_user_projects() -> list[str]:
    """枚举所有 ai-user 的 home 目录（即项目目录）。
    
    ai-user 命名规则: i{main_user}_{项目标识}，如 imaxx_aimirror、imaxx_e3460d。
    其 home 目录就是项目路径，.am_status 直接位于其中。
    """
    main_user = "maxx"
    prefix = f"i{main_user}_"
    projects: list[str] = []

    try:
        with open("/etc/passwd", "r") as f:
            for line in f:
                parts = line.strip().split(":")
                if len(parts) < 6:
                    continue
                username = parts[0]
                home = parts[5]
                # 匹配 ai-user: i{main_user}_ 前缀
                if username.startswith(prefix):
                    status_path = os.path.join(home, ".am_status")
                    if os.path.exists(status_path):
                        projects.append(home)
    except OSError as e:
        print(f"[ERROR] 读取 /etc/passwd 失败: {e}", file=sys.stderr)
        return []

    return sorted(projects)


def scan_all_status() -> list[dict]:
    """枚举所有 ai-user 项目，检查每个 .am_status。"""
    print(f"枚举 ai-user 项目 (prefix=imaxx_) ...\n")
    project_dirs = enumerate_ai_user_projects()
    print(f"找到 {len(project_dirs)} 个 ai-user 项目\n")

    if not project_dirs:
        print("[ERROR] 未找到任何 ai-user 项目", file=sys.stderr)
        return []
    projects = []

    for project_dir in project_dirs:
        project_name = os.path.basename(project_dir)

        status = read_status(project_dir)
        if status is None:
            continue

        mounts = status.get("mounts", [])
        if not mounts:
            continue

        main_user = status.get("main_user", "")
        expected_home = get_main_user_home(main_user) if main_user else ""

        mounts_issues = []
        for mount in mounts:
            issues = check_mount_entry(mount, project_dir, main_user, expected_home)
            if issues:
                mounts_issues.append((mount, issues))

        projects.append({
            "path": project_dir,
            "name": project_name,
            "main_user": main_user,
            "expected_home": expected_home,
            "total": len(mounts),
            "issues": mounts_issues,
            "broken": len(mounts_issues),
        })

    return projects


def print_scan_summary(projects: list[dict]):
    """打印扫描汇总。"""
    total_broken = sum(1 for p in projects if p["broken"] > 0)
    total_ok = len(projects) - total_broken

    print()
    print("=" * 60)
    print(f"扫描结果: {len(projects)} 个项目有 mount 条目")
    print(f"  正常: {total_ok}")
    print(f"  异常: {total_broken}")
    print("=" * 60)

    if total_broken == 0:
        return

    print()
    for p in projects:
        if p["broken"] == 0:
            continue
        print(f"❌ {p['name']} ({p['path']})")
        print(f"   主用户: {p['main_user']}  home: {p['expected_home']}")
        print(f"   异常: {p['broken']}/{p['total']}")
        for src, _ in p["issues"][:3]:
            print(f"     source={src.get('source','?')}")
        if p["broken"] > 3:
            print(f"     ... 还有 {p['broken']-3} 条")
        print()


def raise_batch_issue(projects: list[dict], all_errors: list[str],
                      auto_fix_tried: bool, auto_fix_failed: bool):
    """向 aimirror 发布批量异常 issue。"""
    broken = [p for p in projects if p["broken"] > 0]
    total_broken_projects = len(broken)
    total_broken_mounts = sum(p["broken"] for p in broken)

    body_lines = [
        f"## 🔴🔴🔴 严重问题: {total_broken_projects} 个项目 .am_status 异常\n",
        f"扫描 {DEV_SCAN_ROOT} 下所有项目，发现 **{total_broken_projects} 个项目** 存在 **{total_broken_mounts} 条** mount 路径错误。\n",
    ]

    if all_errors:
        body_lines.append("### 更新异常\n")
        for e in all_errors:
            body_lines.append(f"- ❌ {e}")
        body_lines.append("")

    if auto_fix_tried:
        if auto_fix_failed:
            body_lines.append("### 🚨 am auto-fix-all 执行失败\n\n自动修复机制已尝试但未能执行成功。必须人工介入。\n")
        else:
            body_lines.append("### ⚠ am auto-fix-all 已执行但仍有残留\n\n自动修复已执行，但以下项目仍有异常未修复：\n")

    body_lines.append("### 异常项目列表\n")
    for p in broken:
        body_lines.append(f"#### {p['name']} ({p['broken']}/{p['total']})")
        body_lines.append(f"- 路径: `{p['path']}`")
        body_lines.append(f"- 主用户: {p['main_user']}  home: {p['expected_home']}")
        for src, issues in p["issues"][:5]:
            s = src.get("source", "?")
            t = src.get("target", "?")
            body_lines.append(f"- source: `{s}`")
            for iss in issues[:2]:
                body_lines.append(f"  - ❌ {iss}")
        if p["broken"] > 5:
            body_lines.append(f"  - ... 还有 {p['broken']-5} 条")
        body_lines.append("")

    body_lines.append("### 🔴 就是代码 BUG，不接受任何辩解\n")
    body_lines.append("所有 source 都是 `/usr/maxx/...`，但该目录根本不存在。")
    body_lines.append("**驳回以下借口：**")
    body_lines.append("- ❌ \"旧版本遗留\" → 当前代码生成的 `.am_status` 出错就是当前代码的 BUG")
    body_lines.append("- ❌ \"mountinfo 回退\" → 治标不治本，错误数据就不该被写入")
    body_lines.append("- ❌ \"需要跑 am update\" → AI user 不能跑 am update，所以代码必须一开始就写对")
    body_lines.append("- ❌ \"需要加 group\" → AI user 禁止在 ai-mirror group，设计如此\n")
    body_lines.append("**事实：代码写 `.am_status` 时用了错误的主用户 home 路径。这就是 bug。**\n")

    body_lines.append("### ⚡ 强制命令（P0 · 今天必须搞定）\n")
    body_lines.append("1. **立刻修** 写 `.am_status` 的代码，source 路径必须正确")
    body_lines.append("2. **删掉** mountinfo 回退这类补丁，从源头修")
    body_lines.append("3. **今天内**: 修 → 编译 → 测试 → `am update`（maxx 执行）→ 再次扫描验证异常归零")
    body_lines.append("4. **没修好之前 issue 不关**，下次扫描还有异常继续发\n")
    body_lines.append("> **P0。AI user 没有权限，所以代码必须写对。没有借口，没有延期。修代码。**")

    title = f"🔴🔴🔴 [P0] 批量 .am_status 异常 — {total_broken_projects} 个项目 {total_broken_mounts} 条路径错误"
    if auto_fix_tried and not auto_fix_failed:
        title += "（auto-fix 未能全部修复）"

    AIMIRROR_REPO = os.path.expanduser("~maxx/dev/aimirror/ai-mirror")
    aimirror_issues_dir = os.path.join(AIMIRROR_REPO, "issues")
    if not os.path.isdir(aimirror_issues_dir):
        print(f"[ERROR] 找不到 aimirror issues 目录: {aimirror_issues_dir}", file=sys.stderr)
        return

    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    tags_yaml = "\n".join(f"  - {t}" for t in ["am", "mount", "broken", "P0", "bug", "batch"])

    issue_content = f"""---
source: check-am-fix-amount.py
severity: critical
createdAt: {timestamp}
tags:
{tags_yaml}
---

# {title}

{"\n".join(body_lines)}

---
*自动批量扫描 by check-am-fix-amount.py | {timestamp}*
"""

    tmp_issues_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tmp-issues")
    os.makedirs(tmp_issues_dir, exist_ok=True)
    issue_path = os.path.join(tmp_issues_dir, f"batch-am-status-broken-{int(time.time())}.md")

    try:
        with open(issue_path, "w") as f:
            f.write(issue_content)
        print(f"[INFO] Issue 文件已创建: {issue_path}")
    except OSError as e:
        print(f"[ERROR] 无法创建 issue 文件: {e}", file=sys.stderr)
        return

    try:
        result = subprocess.run(
            [os.path.expanduser("~/.local/bin/raise-issue"), issue_path, aimirror_issues_dir],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode == 0:
            print(f"[INFO] Issue 已发布到: {aimirror_issues_dir}")
        else:
            print(f"[ERROR] 发布 issue 失败 (code={result.returncode}): {result.stderr.strip()}", file=sys.stderr)
    except FileNotFoundError:
        print("[ERROR] raise-issue 工具未找到", file=sys.stderr)
    except subprocess.TimeoutExpired:
        print("[ERROR] raise-issue 执行超时", file=sys.stderr)


def main():
    all_errors: list[str] = []

    # === 第一步：更新 ai-mirror ===
    update_errors = update_ai_mirror()
    all_errors.extend(update_errors)

    # 如果有 install.sh 失败，单独发 issue
    install_error = [e for e in update_errors if "install.sh" in e]
    if install_error:
        print("[ISSUE] install.sh 执行出错，向 aimirror 发布 issue ...")
        body = [
            "## 🔴🔴🔴 严重问题: install.sh 挂了\n",
            "install.sh 是 ai-mirror 的安装脚本。它挂了，整个工具链不可用。\n",
        ]
        for e in install_error:
            body.append(f"- ❌ **{e}**")
        body.extend([
            "",
            "### ⚡ 命令（P0 · 今天解决）",
            "1. **立刻查** 为什么 install.sh 会失败",
            "2. **今天修好** 重新发布",
            "3. **修完跑本脚本验证**",
            "",
            "> **P0。不修好所有 ai-user 项目都无法更新。别拖。**",
        ])
        AIMIRROR_REPO = os.path.expanduser("~maxx/dev/aimirror/ai-mirror")
        aimirror_issues_dir = os.path.join(AIMIRROR_REPO, "issues")
        if os.path.isdir(aimirror_issues_dir):
            timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            tags_yaml = "\n".join("  - " + t for t in ["am", "install", "error", "P0", "bug"])
            issue_content = f"""---
source: check-am-fix-amount.py
severity: critical
createdAt: {timestamp}
tags:
{tags_yaml}
---

# 🔴🔴🔴 [P0] install.sh 挂了 — ai-mirror，代码 BUG，立刻修！

{chr(10).join(body)}

---
*自动报告 by check-am-fix-amount.py | {timestamp}*
"""
            issue_path = os.path.join(tmp_issues_dir, f"install-failed-{int(time.time())}.md")
            os.makedirs(os.path.dirname(issue_path), exist_ok=True)
            with open(issue_path, "w") as f:
                f.write(issue_content)
            subprocess.run(
                [os.path.expanduser("~/.local/bin/raise-issue"), issue_path, aimirror_issues_dir],
                capture_output=True, text=True, timeout=30,
            )

    # === 第二步：扫描所有 .am_status ===
    projects = scan_all_status()
    if not projects:
        print("[ERROR] 未扫描到任何 .am_status", file=sys.stderr)
        return 1

    broken_projects = [p for p in projects if p["broken"] > 0]

    # === 第三步：如果发现异常，尝试 am auto-fix-all ===
    auto_fix_tried = False
    auto_fix_failed = False
    if broken_projects:
        print()
        print(f"[INFO] 发现 {len(broken_projects)} 个项目异常，尝试 am auto-fix-all ...")
        result = run_cmd(["am", "auto-fix-all"], timeout=120)
        auto_fix_tried = True
        if result is not None and result.returncode == 0:
            print(f"  ✓ am auto-fix-all 执行成功")
        else:
            code = result.returncode if result else "N/A"
            print(f"  ⚠ am auto-fix-all 执行结果 (code={code})")
            if result and result.stderr.strip():
                print(f"  stderr: {result.stderr.strip()[:200]}")
            if result and "error" in result.stderr.lower():
                auto_fix_failed = True

        # 重新扫描验证
        print(f"\n[INFO] 重新扫描验证修复效果 ...")
        projects2 = scan_all_status()
        if projects2:
            broken2 = [p for p in projects2 if p["broken"] > 0]
            fixed_count = len(broken_projects) - len(broken2)
            if fixed_count > 0:
                print(f"  ✓ 已修复 {fixed_count} 个项目")
            projects = projects2
            broken_projects = broken2
        print()

    # === 第四步：输出汇总 ===
    print_scan_summary(projects)

    # === 第五步：仍有异常 → 发 issue ===
    if broken_projects:
        raise_batch_issue(projects, all_errors, auto_fix_tried, auto_fix_failed)
    elif all_errors and not install_error:
        # 有更新错误但 mount 没问题
        pass

    has_any_error = len(all_errors) > 0 or len(broken_projects) > 0
    if not has_any_error:
        print("[OK] 全部正常")
        return 0
    return 1


if __name__ == "__main__":
    from pathlib import Path  # noqa: F811
    tmp_issues_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tmp-issues")
    sys.exit(main())
