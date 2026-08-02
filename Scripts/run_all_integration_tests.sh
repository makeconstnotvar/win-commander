#!/usr/bin/env bash

set -euo pipefail

command -v xcodebuild >/dev/null 2>&1 || {
  echo "xcodebuild is required." >&2
  exit 1
}
command -v docker >/dev/null 2>&1 || {
  echo "docker is required: https://www.docker.com" >&2
  exit 1
}
command -v nc >/dev/null 2>&1 || {
  echo "nc is required to verify Docker fixture readiness." >&2
  exit 1
}
docker info >/dev/null 2>&1 || {
  echo "Docker daemon is not available." >&2
  exit 1
}

export LC_CTYPE="${LC_CTYPE:-en_US.UTF-8}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_container_overflow=0}"

scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
repo_root="$(cd "$scripts_dir/.." >/dev/null 2>&1 && pwd)"
fixtures_dir="$repo_root/Source/VFS/tests/data/docker"
project="$repo_root/Source/WinCommander/WinCommander.xcodeproj"
host_arch="$(uname -m)"
test_seed="${WINCOMMANDER_TEST_SEED:-424242}"

owns_build_dir=0
if [[ -n "${WINCOMMANDER_INTEGRATION_TEST_BUILD_DIR:-}" ]]; then
  build_dir="$WINCOMMANDER_INTEGRATION_TEST_BUILD_DIR"
  mkdir -p "$build_dir"
else
  build_dir="$(mktemp -d "${TMPDIR:-/tmp}/wincommander-integration-tests.XXXXXX")"
  owns_build_dir=1
fi

cleanup() {
  "$fixtures_dir/stop.sh"
  if [[ "$owns_build_dir" -eq 1 && "${WINCOMMANDER_KEEP_BUILD_ARTIFACTS:-0}" != "1" ]]; then
    rm -rf "$build_dir"
  else
    echo "Integration-test artifacts: $build_dir"
  fi
}
trap cleanup EXIT

echo "Starting Docker integration fixtures"
"$fixtures_dir/stop.sh"
"$fixtures_dir/start.sh"

wait_for_port() {
  local port="$1"
  for _ in {1..100}; do
    if nc -z 127.0.0.1 "$port" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "Docker fixture did not open port $port." >&2
  return 1
}

wait_for_port 9021
wait_for_port 9022
wait_for_port 9080

xcode=(
  xcodebuild
  -project "$project"
  -scheme IntegrationTests
  -configuration Debug
  -destination "platform=macOS,arch=$host_arch"
  -derivedDataPath "$build_dir/DerivedData"
  -parallelizeTargets
  -enableAddressSanitizer YES
  ONLY_ACTIVE_ARCH=YES
  COMPILER_INDEX_STORE_ENABLE=NO
  CODE_SIGNING_ALLOWED=NO
)

settings_file="$build_dir/build-settings.txt"
if ! "${xcode[@]}" -showBuildSettings >"$settings_file" 2>/dev/null; then
  echo "Unable to read build settings for the IntegrationTests scheme." >&2
  exit 1
fi

test_names=()
binary_paths=()
while IFS=$'\t' read -r target products_dir product_name; do
  [[ -n "$target" && -n "$products_dir" && -n "$product_name" ]] || continue
  test_names+=("$target")
  binary_paths+=("$products_dir/$product_name")
done < <(
  awk '
    /^Build settings for action build and target / {
      target = $NF
      sub(/:$/, "", target)
    }
    / BUILT_PRODUCTS_DIR = / {
      products_dir = $0
      sub(/^.* = /, "", products_dir)
    }
    / FULL_PRODUCT_NAME = / {
      product_name = $0
      sub(/^.* = /, "", product_name)
      print target "\t" products_dir "\t" product_name
    }
  ' "$settings_file"
)

if [[ "${#binary_paths[@]}" -eq 0 ]]; then
  echo "No integration-test products were discovered from the IntegrationTests scheme." >&2
  exit 1
fi

log_file="$build_dir/xcodebuild.log"
echo "Building aggregate IntegrationTests (Debug ASAN, $host_arch)"
if command -v xcpretty >/dev/null 2>&1; then
  "${xcode[@]}" build 2>&1 | tee "$log_file" | xcpretty
else
  "${xcode[@]}" -quiet build 2>&1 | tee "$log_file"
fi

cd "$repo_root"
for index in "${!binary_paths[@]}"; do
  binary_path="${binary_paths[$index]}"
  test_name="${test_names[$index]}"
  if [[ ! -x "$binary_path" ]]; then
    echo "Built integration-test product is not executable: $binary_path" >&2
    exit 1
  fi
  echo "Running $test_name"
  "$binary_path" --rng-seed "$test_seed"
done

echo "All aggregate integration tests passed."
