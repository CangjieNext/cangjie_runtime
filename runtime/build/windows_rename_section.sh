#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

get_sections() {
    local file="$1"
    if command -v objdump >/dev/null 2>&1; then
        objdump -h "$file" 2>/dev/null | \
            awk '/^ +[0-9]+ / {print $2}' | \
            grep -E '^\.text' || true
        return $?
    fi
}

process_simple_merge() {
  local input_file="$1"
  sections=$(get_sections "$input_file")
  
  if [ -z "$sections" ]; then
    return 0
  fi
  
  local args=()
  while IFS= read -r section; do
    if [[ -n "$section" ]]; then
      args+=("--rename-section" "$section=.text_rt")
    fi
  done <<< "$sections"
  
  if [ "${#args[@]}" -gt 0 ]; then
    objcopy "${args[@]}" "$input_file"
  fi

  merge_duplicate_sections "$input_file"
  return 0
}

merge_duplicate_sections() {
    local obj_file="$1"
    local rt_sections=$(objdump -h "$obj_file" 2>/dev/null | \
                       awk '/^ +[0-9]+ / && $2 == ".text_rt" {print $1}' | \
                       tr -d ' ')
    
    local rt_count=$(echo "$rt_sections" | wc -w)
    if [ "$rt_count" -le 1 ]; then
        return 0
    fi
    
    local temp_file="${obj_file}.tmp"
    if objcopy --help 2>&1 | grep -q "merge-section"; then
        for ((i=1; i<rt_count; i++)); do
            objcopy --merge-section .text_rt=.text_rt "$obj_file" "$temp_file" 2>/dev/null
            mv "$temp_file" "$obj_file" 2>/dev/null
        done
    else
        local temp_dir=$(mktemp -d)
        objcopy --only-section=.text_rt "$obj_file" "${temp_dir}/text_rt.o" 2>/dev/null
        objcopy --remove-section=.text_rt "$obj_file" "${temp_dir}/without_text_rt.o" 2>/dev/null
        objcopy --add-section=.text_rt="${temp_dir}/text_rt.o" \
                "${temp_dir}/without_text_rt.o" "$obj_file" 2>/dev/null
        rm -rf "$temp_dir"
    fi
}

process_static_library() {
  local lib_file="$1"
  local temp_dir=$(mktemp -d)
  local current_dir=$(pwd)
  cd "$temp_dir"
  if ! ar x "$lib_file" 2>/dev/null; then
    return 1
  fi

  for obj in "$temp_dir"/*.obj; do
      [ -f "$obj" ] || continue
      obj_abs=$(realpath "$obj")
      process_simple_merge "$obj_abs"
  done
  
  rm -f "$lib_file"
  pushd "$temp_dir" >/dev/null
  ar rcs "$lib_file"  *.obj 2>/dev/null
  popd >/dev/null
  
  return 0
}

input_file=$1
echo "rename and merge section, input: ${input_file}, output: ${input_file}"
process_static_library "$input_file"