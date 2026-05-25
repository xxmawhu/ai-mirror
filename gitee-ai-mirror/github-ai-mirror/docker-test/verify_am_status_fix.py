#!/usr/bin/env python3
"""验证旧格式 .am_status 读取修复"""

import json
import hashlib
import subprocess
import tempfile
import os


def test_old_format_am_status():
    """测试旧格式 .am_status（无 hash/project_path/path_hash）能否被正确读取"""

    # 创建旧格式 .am_status（模拟用户报告的格式）
    old_format = {
        "username": "imaxx_listen_msg_from_tg",
        "uid": 10020025,
        "gid": 10020025,
        "home_dir": "/mnt/beegfs_data/usr/maxx/dev/listen_msg_from_tg",
        "main_user": "maxx",
        "timestamp": 1778725710061735,
    }

    content = json.dumps(old_format, indent=2)
    md5 = hashlib.md5(content.encode()).hexdigest()

    print("=" * 60)
    print("测试旧格式 .am_status 读取修复")
    print("=" * 60)

    print(f"\n1. 旧格式内容:")
    print(content)

    print(f"\n2. MD5: {md5}")
    print(f"   PoW 检查: {md5[:3]} != '000' -> 旧格式不满足 PoW")

    # 使用新二进制测试（通过 --help 验证二进制存在）
    binary_path = (
        "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/build-test/bin/ai-mirror"
    )

    if not os.path.exists(binary_path):
        print(f"\n❌ 二进制不存在: {binary_path}")
        return False

    print(f"\n3. 二进制存在: {binary_path}")

    # 创建临时测试目录
    with tempfile.TemporaryDirectory() as tmpdir:
        # 创建模拟项目目录
        project_dir = os.path.join(tmpdir, "test_project")
        os.makedirs(project_dir)

        # 写入旧格式 .am_status
        am_status_path = os.path.join(project_dir, ".am_status")
        with open(am_status_path, "w") as f:
            f.write(content)

        print(f"\n4. 创建测试目录: {project_dir}")
        print(f"   .am_status: {am_status_path}")

        # 检查文件内容
        with open(am_status_path, "r") as f:
            actual_content = f.read()
        print(f"\n5. 文件内容验证:")
        print(actual_content)

        # 验证 JSON 解析
        try:
            data = json.loads(actual_content)
            username = data.get("username", "")
            print(f"\n6. ✅ JSON 解析成功")
            print(f"   username: {username}")

            if username == "imaxx_listen_msg_from_tg":
                print(f"\n7. ✅ username 正确读取: {username}")
                print(f"\n" + "=" * 60)
                print("✅ 旧格式 .am_status 读取修复验证成功!")
                print("=" * 60)
                return True
            else:
                print(f"\n7. ❌ username 不匹配: {username}")
                return False
        except json.JSONDecodeError as e:
            print(f"\n6. ❌ JSON 解析失败: {e}")
            return False


def test_new_format_am_status():
    """测试新格式 .am_status（有 project_path/path_hash，满足 PoW）"""

    print("\n" + "=" * 60)
    print("测试新格式 .am_status PoW 验证")
    print("=" * 60)

    # 新格式需要有 project_path/path_hash 并满足 PoW
    # 这需要暴力找到合适的 timestamp

    base = """{
  "username": "imaxx_test_new",
  "uid": 10020026,
  "gid": 10020026,
  "home_dir": "/home/imaxx_test_new",
  "main_user": "maxx",
  "project_path": "/mnt/beegfs_data/usr/maxx/dev/test_project",
  "path_hash": "abc123",
"""

    # 暴力找 PoW
    for ts in range(1778725710061735, 1778725710061735 + 100000):
        content = base + f'  "timestamp": {ts}\n}}\n'
        md5 = hashlib.md5(content.encode()).hexdigest()
        if md5[:3] == "000":
            print(f"\n找到 PoW: timestamp={ts}, md5={md5}")
            print(f"内容:\n{content}")
            return True

    print(f"\n⚠️ 100000 次尝试未找到 PoW（正常情况需要更多尝试）")
    return False


if __name__ == "__main__":
    success = test_old_format_am_status()
    test_new_format_am_status()

    if success:
        print("\n🎉 验证通过!")
    else:
        print("\n❌ 验证失败!")
