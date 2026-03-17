#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set -e

# Rename .text section to .text_rt in object files
# This should be done before linking object files into static library
# Usage: windows_rename_section.sh <obj_file1> <obj_file2> ...

for obj in "$@"; do
	if [ -f "$obj" ]; then
		echo "Renaming section in $obj"
		objcopy --rename-section .text=.text_rt "$obj" "$obj"
	fi
done
