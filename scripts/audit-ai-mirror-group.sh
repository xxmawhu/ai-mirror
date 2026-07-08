#!/usr/bin/env bash
# === audit-ai-mirror-group.sh ===
# 审计 ai-mirror 组成员，检测非法 AI 用户
# 用途：检测 ai-mirror 组是否被非法 AI 用户污染
# 运行方式：sudo bash scripts/audit-ai-mirror-group.sh
# 输出：发现异常时以 JSON 格式报告

set -euo pipefail

GROUP_NAME="ai-mirror"
LEGIT_MEMBERS="maxx sch chensi maxx_ai"

# 获取当前组成员
GROUP_MEMBERS=$(getent group "$GROUP_NAME" | cut -d: -f4 | tr ',' '\n' | sort)

# 分离合法和非法成员
UNAUTHORIZED=()
AUTHORIZED=()

while IFS= read -r user; do
	[[ -z "$user" ]] && continue
	if echo "$LEGIT_MEMBERS" | tr ' ' '\n' | grep -qxF "$user"; then
		AUTHORIZED+=("$user")
	else
		UNAUTHORIZED+=("$user")
	fi
done <<<"$GROUP_MEMBERS"

# 输出结果
echo "{"
echo "  \"group\": \"$GROUP_NAME\","
echo "  \"total_members\": ${#AUTHORIZED[@]} + ${#UNAUTHORIZED[@]},"
echo "  \"authorized_count\": ${#AUTHORIZED[@]},"
echo "  \"unauthorized_count\": ${#UNAUTHORIZED[@]},"
echo "  \"authorized_members\": ["
for i in "${!AUTHORIZED[@]}"; do
	sep=","
	[[ $i -eq $((${#AUTHORIZED[@]} - 1)) ]] && sep=""
	echo "    \"${AUTHORIZED[$i]}\"$sep"
done
echo "  ],"
echo "  \"unauthorized_members\": ["
for i in "${!UNAUTHORIZED[@]}"; do
	sep=","
	[[ $i -eq $((${#UNAUTHORIZED[@]} - 1)) ]] && sep=""
	echo "    \"${UNAUTHORIZED[$i]}\"$sep"
done
echo "  ]"
echo "}"

# 退出码：有非法成员时返回 1
if [[ ${#UNAUTHORIZED[@]} -gt 0 ]]; then
	echo "[SECURITY] WARNING: $GROUP_NAME group has ${#UNAUTHORIZED[@]} unauthorized members!" >&2
	exit 1
fi

exit 0
