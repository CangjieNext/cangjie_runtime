#!/usr/bin/env bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# Run the LinkedBlockingQueue benchmark (after build.sh).
set -euo pipefail
cd "$(dirname "$0")"
if [ -x ./lbq_bench ]; then
  ./lbq_bench
elif [ -f ./lbq_bench.exe ]; then
  ./lbq_bench.exe
else
  echo "lbq_bench not built; run build.sh first" >&2
  exit 1
fi
