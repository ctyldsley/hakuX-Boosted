#!/usr/bin/env bash
# hakuX-Boosted: builds the PowerPC assembler the instruction tests need:
# binutils 2.24 with third_party/binutils' patch for the Xbox 360's VMX128
# instructions. The same recipe as third_party/binutils/build.sh, for a
# target directory of the caller's choice and with the source checked
# against a known hash.
#
#   build-binutils.sh <install prefix>
set -euo pipefail
prefix=$1
here=$(cd "$(dirname "$0")" && pwd)
patch_file=$(cd "$here/../../../../../../third_party/binutils" && pwd)/binutils-2.24-vmx128.patch
sha=4930b2886309112c00a279483eaef2f0f8e1b1b62010e0239c16b22af7c346d4
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
ok=0
for url in \
  http://archive.ubuntu.com/ubuntu/pool/main/b/binutils/binutils_2.24.orig.tar.gz \
  https://old-releases.ubuntu.com/ubuntu/pool/main/b/binutils/binutils_2.24.orig.tar.gz \
  https://ftp.gnu.org/gnu/binutils/binutils-2.24.tar.gz \
  https://sourceware.org/pub/binutils/releases/binutils-2.24.tar.gz; do
  if curl -fsSL -m 300 "$url" -o binutils.tar.gz \
     && echo "$sha  binutils.tar.gz" | sha256sum -c - > /dev/null 2>&1; then
    echo "binutils 2.24 source from $url"
    ok=1
    break
  fi
  echo "not usable: $url"
done
if [ "$ok" != 1 ]; then echo "::error::no source for binutils 2.24 with the expected hash"; exit 1; fi
tar zxf binutils.tar.gz
cd binutils-2.24
patch -p0 < "$patch_file"
./configure --disable-debug --disable-dependency-tracking --disable-werror \
  --disable-nls --target=powerpc-none-elf --with-gnu-ld --with-gnu-as \
  --prefix="$prefix" > "$work/configure.log" 2>&1 || { tail -40 "$work/configure.log"; exit 1; }
# MAKEINFO=true: the documentation is not wanted and 2.24's does not build
# with a current texinfo.
make -j"$(nproc)" MAKEINFO=true > "$work/make.log" 2>&1 || { tail -60 "$work/make.log"; exit 1; }
make install MAKEINFO=true > "$work/install.log" 2>&1 || { tail -40 "$work/install.log"; exit 1; }
"$prefix/bin/powerpc-none-elf-as" --version | head -1
