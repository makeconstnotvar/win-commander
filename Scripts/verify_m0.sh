#!/usr/bin/env bash

set -euo pipefail

command -v xcodebuild >/dev/null 2>&1 || {
  echo "xcodebuild is required." >&2
  exit 1
}

scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
repo_root="$(cd "$scripts_dir/.." && pwd)"
project="$repo_root/Source/WinCommander/WinCommander.xcodeproj"
host_arch="$(uname -m)"

owns_build_dir=0
if [[ -n "${WINCOMMANDER_M0_BUILD_DIR:-}" ]]; then
  build_dir="$WINCOMMANDER_M0_BUILD_DIR"
  mkdir -p "$build_dir"
else
  build_dir="$(mktemp -d "${TMPDIR:-/tmp}/wincommander-m0.XXXXXX")"
  owns_build_dir=1
fi

cleanup() {
  if [[ "$owns_build_dir" -eq 1 && "${WINCOMMANDER_KEEP_BUILD_ARTIFACTS:-0}" != "1" ]]; then
    rm -rf "$build_dir"
  else
    echo "M0 verification artifacts: $build_dir"
  fi
}
trap cleanup EXIT

if [[ -n "${DEVELOPER_DIR:-}" && ! -x "$DEVELOPER_DIR/usr/bin/xcodebuild" ]]; then
  echo "DEVELOPER_DIR does not contain an executable xcodebuild: $DEVELOPER_DIR" >&2
  exit 1
fi
echo "Toolchain"
toolchain_version="$(xcodebuild -version)"
echo "$toolchain_version"
actual_xcode_version="$(awk 'NR == 1 && $1 == "Xcode" { print $2 }' <<<"$toolchain_version")"
if [[ "$actual_xcode_version" != "26.5" ]]; then
  echo "M0 requires Xcode 26.5, found ${actual_xcode_version:-unknown}." >&2
  exit 1
fi

echo "Validating project and schemes"
xcodebuild -project "$project" -list >/dev/null

echo "Building WinCommander-Unsigned (Debug, $host_arch)"
xcodebuild \
  -project "$project" \
  -scheme WinCommander-Unsigned \
  -configuration Debug \
  -destination "platform=macOS,arch=$host_arch" \
  -derivedDataPath "$build_dir/AppDerivedData" \
  -parallelizeTargets \
  -quiet \
  ONLY_ACTIVE_ARCH=YES \
  COMPILER_INDEX_STORE_ENABLE=NO \
  CODE_SIGNING_ALLOWED=NO \
  build 2>&1 | tee "$build_dir/unsigned-debug-build.log"

echo "Running aggregate unit tests"
WINCOMMANDER_UNIT_TEST_BUILD_DIR="$build_dir/UnitTests" \
  "$scripts_dir/run_all_unit_tests.sh" Debug

echo "M0 verification passed."
