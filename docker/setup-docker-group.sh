#!/bin/bash
set -euo pipefail

sudo usermod -aG docker maxx_ai
echo "maxx_ai 已添加到 docker 组"
echo "请运行 'newgrp docker' 或重新登录以生效"
