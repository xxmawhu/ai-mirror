#!/usr/bin/env python3
"""
patch-am-status.py — 批量修复 .am_status 文件中的 source 路径与 path_hash

问题背景:
  am 工具旧版本写入 .am_status 时 source 路径使用 /usr/maxx/（不存在），
  而非正确的 /mnt/beegfs_data/usr/maxx/。代码已修复但存量文件需要修正。

本脚本功能:
  1. 扫描 /mnt/beegfs_data/usr/maxx/dev/ 下所有 .am_status 文件
  2. 替换 mount source 路径: /usr/maxx/ → /mnt/beegfs_data/usr/maxx/
  3. 补全空 path_hash（从 project_path 计算 sha256[:6]）
  4. 重新执行 PoW（Proof of Work: md5 前缀 "000"）

使用方法:
  sudo -u root python3 scripts/patch-am-status.py
  # 需要 root 因为有文件属 root:root
  # 也可以用 python3 scripts/patch-am-status.py --dry-run 预览

依赖:
  - Python 3.8+
  - hashlib (标准库)
  - json (标准库)
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

DEV_ROOT = Path("/mnt/beegfs_data/usr/maxx/dev")
WRONG_PREFIX = "/usr/maxx/"
CORRECT_PREFIX = "/mnt/beegfs_data/usr/maxx"
LOG_DIR = Path("/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/log/fix")


def md5_hex(s: str) -> str:
    return hashlib.md5(s.encode("utf-8")).hexdigest()


def sha256_prefix(s: str, length: int = 6) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()[:length]


def make_state_content(info: dict, main_user: str) -> str:
    """
    重新生成 .am_status 内容（无 PoW）。
    PoW(md5) 校验已被移除（没有安全价值，且导致 am update 拒绝合法文件）。
    """
    # 保持字段顺序: username, uid, gid, home_dir, main_user, project_path, path_hash, mounts
    j = {
        "username": info.get("username", ""),
        "uid": info.get("uid", 0),
        "gid": info.get("gid", 0),
        "home_dir": info.get("home_dir", ""),
        "main_user": main_user,
        "project_path": info.get("project_path", info.get("home_dir", "")),
        "path_hash": info.get("path_hash", ""),
        "mounts": info.get("mounts", []),
    }
    return json.dumps(j, indent=2, ensure_ascii=False) + "\n"


def fix_source_path(source: str) -> str:
    """Replace /usr/maxx/ -> /mnt/beegfs_data/usr/maxx/ in source paths."""
    if source.startswith(WRONG_PREFIX):
        return source.replace(WRONG_PREFIX, CORRECT_PREFIX + "/", 1)
    return source


def compute_path_hash(project_path: str) -> str:
    """Compute 6-char path_hash from project_path."""
    if not project_path:
        return ""
    return sha256_prefix(project_path)


def fix_am_status(filepath: Path, dry_run: bool = False) -> dict:
    """
    Fix a single .am_status file.
    Returns a dict with status info.
    """
    result = {"file": str(filepath), "fixed": False, "changes": []}

    try:
        with open(filepath, "r") as f:
            content = f.read()
    except (PermissionError, FileNotFoundError) as e:
        result["error"] = f"read failed: {e}"
        return result

    # Parse JSON
    try:
        data = json.loads(content)
    except json.JSONDecodeError as e:
        result["error"] = f"invalid JSON: {e}"
        return result

    # --- Fix 1: sources in mounts ---
    mounts = data.get("mounts", [])
    mount_fixed = False
    for mount in mounts:
        src = mount.get("source", "")
        fixed = fix_source_path(src)
        if fixed != src:
            mount["source"] = fixed
            mount_fixed = True
            result["changes"].append(f"source: {src} -> {fixed}")

    # --- Fix 2: path_hash ---
    old_hash = data.get("path_hash", "")
    project_path = data.get("project_path", "")
    if not old_hash and project_path:
        new_hash = compute_path_hash(project_path)
        if new_hash:
            data["path_hash"] = new_hash
            result["changes"].append(f"path_hash: '' -> {new_hash}")

    # --- Fix 3: also check project_path itself for wrong prefix ---
    pp = data.get("project_path", "")
    if pp and pp.startswith(WRONG_PREFIX):
        fixed_pp = fix_source_path(pp)
        data["project_path"] = fixed_pp
        result["changes"].append(f"project_path: {pp} -> {fixed_pp}")

    # --- Fix 4: home_dir too ---
    hd = data.get("home_dir", "")
    if hd and hd.startswith(WRONG_PREFIX):
        fixed_hd = fix_source_path(hd)
        data["home_dir"] = fixed_hd
        result["changes"].append(f"home_dir: {hd} -> {fixed_hd}")

    if not result["changes"]:
        result["skipped"] = True
        return result

    # Regenerate content with PoW
    main_user = data.get("main_user", "")
    new_content = make_state_content(data, main_user)

    if dry_run:
        result["fixed"] = True
        result["dry_run"] = True
        return result

    # Write back
    try:
        with open(filepath, "w") as f:
            f.write(new_content)
        result["fixed"] = True
    except PermissionError as e:
        result["error"] = f"write failed: {e}"

    return result


def main():
    parser = argparse.ArgumentParser(
        description="批量修复 .am_status 文件中的 source 路径与 path_hash"
    )
    parser.add_argument(
        "--dry-run", "-n",
        action="store_true",
        help="预览模式，不实际写入文件"
    )
    parser.add_argument(
        "--dev-root",
        default=str(DEV_ROOT),
        help=f"扫描根目录 (默认: {DEV_ROOT})"
    )
    args = parser.parse_args()

    dev_root = Path(args.dev_root)
    if not dev_root.exists():
        print(f"Error: {dev_root} does not exist")
        sys.exit(1)

    print(f"Scanning {dev_root} for .am_status files...")
    status_files = sorted(dev_root.rglob(".am_status"))
    print(f"Found {len(status_files)} .am_status files")

    results = []
    total_changes = 0
    for i, fp in enumerate(status_files):
        result = fix_am_status(fp, dry_run=args.dry_run)
        results.append(result)

        if result.get("skipped"):
            continue

        changes = result.get("changes", [])
        n = len(changes)
        if n > 0:
            total_changes += n
            marker = "[DRY-RUN]" if result.get("dry_run") else "[FIXED]"
            print(f"  {marker} [{i+1}/{len(status_files)}] {fp}")
            for c in changes:
                print(f"         {c}")

        if "error" in result:
            print(f"  [ERROR] [{i+1}/{len(status_files)}] {fp}: {result['error']}")

    # Summary
    fixed = sum(1 for r in results if r.get("fixed"))
    skipped = sum(1 for r in results if r.get("skipped"))
    errors = sum(1 for r in results if "error" in r)

    print(f"\n=== 统计 ===")
    print(f"  总计:     {len(status_files)}")
    print(f"  已修复:   {fixed} ({total_changes} 项变更)")
    print(f"  跳过:     {skipped}")
    print(f"  错误:     {errors}")
    if args.dry_run:
        print(f"\n  预览模式完成。移除 --dry-run 实际执行。")

    # Verify remaining wrong paths
    wrong_remaining = 0
    for fp in status_files:
        try:
            with open(fp, "r") as f:
                c = f.read()
            if WRONG_PREFIX in c:
                wrong_remaining += 1
        except Exception:
            pass

    if wrong_remaining > 0:
        print(f"\n  ⚠ 仍有 {wrong_remaining} 个文件包含 /usr/maxx/ 路径")
    else:
        print(f"\n  ✅ 所有文件均已修复!")

    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
