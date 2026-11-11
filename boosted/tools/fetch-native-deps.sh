#!/usr/bin/env bash
# Fetches the hakuX native core's third-party sources (CMake NC_DEPS) at the
# revisions android/README.md lists, into the directory given (default:
# build/native-deps). Each is a shallow checkout; one already there at the
# right revision is left alone.
#
#   boosted/tools/fetch-native-deps.sh [dir]
#
# Then: cmake -S native -B build/native/core ... -DNC_DEPS=<dir>
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
deps=${1:-$here/build/native-deps}
mkdir -p "$deps"

# fetch <path under NC_DEPS> <repository> <tag or commit> [the commit's first digits]
fetch() {
  local dir="$deps/$1" repo="$2" rev="$3" expect="${4:-}"
  if [ -f "$dir/.hakux-rev" ] && [ "$(cat "$dir/.hakux-rev")" = "$rev" ]; then
    echo "$1: $rev (already there)"
    return
  fi
  rm -rf "$dir"
  mkdir -p "$dir"
  git -C "$dir" init -q
  git -C "$dir" remote add origin "$repo"
  git -C "$dir" fetch -q --depth 1 origin "$rev"
  git -C "$dir" checkout -q FETCH_HEAD
  if [ -n "$expect" ]; then
    local got
    got=$(git -C "$dir" rev-parse 'HEAD^{commit}')
    case "$got" in
      "$expect"*) ;;
      *) echo "$1: $rev is $got, not $expect as android/README.md has it" >&2; exit 1 ;;
    esac
  fi
  echo "$rev" > "$dir/.hakux-rev"
  echo "$1: $rev"
}

fetch volk          https://github.com/zeux/volk.git                                   1.4.304
fetch glslang       https://github.com/KhronosGroup/glslang.git                        16.1.0
fetch spirv_reflect https://github.com/KhronosGroup/SPIRV-Reflect.git                  vulkan-sdk-1.4.328.0
fetch vma           https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git v3.3.0
fetch SDL3          https://github.com/libsdl-org/SDL.git                              release-3.4.18
fetch libslirp      https://gitlab.freedesktop.org/slirp/libslirp.git                  v4.7.0 26be815b
fetch root/subprojects/nv2a_vsh_cpu https://github.com/xemu-project/nv2a_vsh_cpu.git   561fe80da57a881f89000256b459440c0178a7ce

# adrenotools, with its linkernsbypass submodule and hakuX's KGSL draw
# context priority hook (tools/native/adrenotools-kgsl-context-priority.patch)
adreno="$deps/adrenotools"
adreno_rev=8fae8ce254dfc1344527e05301e43f37dea2df80
if [ ! -f "$adreno/.hakux-rev" ] || [ "$(cat "$adreno/.hakux-rev")" != "$adreno_rev+kgsl" ]; then
  rm -rf "$adreno"
  mkdir -p "$adreno"
  git -C "$adreno" init -q
  git -C "$adreno" remote add origin https://github.com/bylaws/libadrenotools.git
  git -C "$adreno" fetch -q --depth 1 origin "$adreno_rev"
  git -C "$adreno" checkout -q FETCH_HEAD
  git -C "$adreno" submodule update -q --init --depth 1
  git -C "$adreno" apply "$here/tools/native/adrenotools-kgsl-context-priority.patch"
  echo "$adreno_rev+kgsl" > "$adreno/.hakux-rev"
fi
echo "adrenotools: $adreno_rev, patched"
echo "NC_DEPS=$deps"
