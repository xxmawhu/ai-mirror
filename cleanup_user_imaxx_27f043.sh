#!/bin/bash
# cleanup_user_imaxx_27f043.sh
# 清理用户 imaxx_27f043 的残留进程并删除用户
# 需要 root 或 sudo 权限执行
#
# 使用方式: sudo bash cleanup_user_imaxx_27f043.sh

set -euo pipefail

USER="imaxx_27f043"
UID="10020138"

echo "=========================================="
echo "清理用户: $USER (UID: $UID)"
echo "=========================================="

# Step 0: 先尝试用 loginctl 终止用户会话（干净关闭 systemd --user）
echo "[Step 0] loginctl terminate-user $USER ..."
loginctl terminate-user "$USER" 2>&1 || echo "  (loginctl 可能需 root, 跳过)"
sleep 2

# Step 1: 强制终止所有残留进程
echo "[Step 1] Killing all processes for $USER ..."
pkill -9 -u "$USER" 2>&1 || echo "  (no processes left)"
sleep 1

# 再次确认无进程残留
REMAINING=$(ps -u "$USER" -o pid= 2>/dev/null || true)
if [ -n "$REMAINING" ]; then
	echo "  WARNING: processes still running: $REMAINING"
	echo "  Force killing with kill -9 ..."
	for pid in $REMAINING; do
		kill -9 "$pid" 2>/dev/null || true
	done
	sleep 1
fi

# Step 2: 清除 crontab
echo "[Step 2] Clearing crontab for $USER ..."
crontab -u "$USER" -r 2>&1 || true

# Step 3: 卸载 bind mounts
echo "[Step 3] Checking for bind mounts for $USER ..."
MOUNTS=$(mount | grep "/$USER/" || true)
if [ -n "$MOUNTS" ]; then
	echo "$MOUNTS" | while IFS= read -r mnt; do
		mnt_point=$(echo "$mnt" | awk '{print $3}')
		umount -f "$mnt_point" 2>/dev/null || true
	done
fi

# Step 4: 删除用户
echo "[Step 4] Removing user $USER ..."
if userdel -r "$USER" 2>&1; then
	echo "  SUCCESS: user $USER removed"
else
	echo "  userdel failed, checking remaining processes..."
	ps -u "$USER" -o pid,comm= 2>/dev/null || true
	exit 1
fi

# Step 5: 清理残留目录
echo "[Step 5] Cleaning up remaining directories ..."
for home in /home/$USER /mnt/beegfs_data/home/$USER; do
	if [ -d "$home" ]; then
		rm -rf "$home" 2>/dev/null && echo "  Removed $home" || echo "  Could not remove $home"
	fi
done

# Step 6: 清理 systemd 用户数据
echo "[Step 6] Cleaning up systemd user data ..."
rm -rf "/run/user/$UID" 2>/dev/null || true
rm -rf "/var/lib/systemd/linger/$USER" 2>/dev/null || true

echo ""
echo "=========================================="
echo "Cleanup of user $USER completed."
echo "=========================================="
