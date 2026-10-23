#!/usr/bin/env bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# Build the LinkedBlockingQueue benchmark with cjc.
# Requires CANGJIE_HOME to point at the Cangjie SDK root and the SDK's
# bin/tools/runtime-lib on PATH (typically via the SDK envsetup script).
set -euo pipefail
cd "$(dirname "$0")"
cjc -o lbq_bench \
  src/block_support.cj \
  src/bq_interface.cj \
  src/blocking_queue.cj \
  src/blocking_queue_upstream.cj \
  src/main.cj
echo "build ok -> ./lbq_bench"
