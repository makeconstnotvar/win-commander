#!/usr/bin/env bash
# Usage: ./run_all_unit_tests.sh [Debug|Release|ASAN|UBSAN]

set -euo pipefail

usage() {
  echo "Usage: $0 [Debug|Release|ASAN|UBSAN]" >&2
}

requested_configuration="${1:-Debug}"
configuration="$requested_configuration"

case "$requested_configuration" in
  Debug|Release)
    ;;
  ASAN)
    configuration="Release"
    ;;
  UBSAN)
    configuration="Release"
    ;;
  *)
    usage
    exit 2
    ;;
esac

command -v xcodebuild >/dev/null 2>&1 || {
  echo "xcodebuild is required." >&2
  exit 1
}

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_container_overflow=0}"
export LC_CTYPE="${LC_CTYPE:-en_US.UTF-8}"

scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
repo_root="$(cd "$scripts_dir/.." && pwd)"
project="$repo_root/Source/WinCommander/WinCommander.xcodeproj"
host_arch="$(uname -m)"
test_seed="${WINCOMMANDER_TEST_SEED:-424242}"

owns_build_dir=0
if [[ -n "${WINCOMMANDER_UNIT_TEST_BUILD_DIR:-}" ]]; then
  build_dir="$WINCOMMANDER_UNIT_TEST_BUILD_DIR"
  mkdir -p "$build_dir"
else
  build_dir="$(mktemp -d "${TMPDIR:-/tmp}/wincommander-unit-tests.XXXXXX")"
  owns_build_dir=1
fi

cleanup() {
  if [[ "$owns_build_dir" -eq 1 && "${WINCOMMANDER_KEEP_BUILD_ARTIFACTS:-0}" != "1" ]]; then
    rm -rf "$build_dir"
  else
    echo "Unit-test artifacts: $build_dir"
  fi
}
trap cleanup EXIT

log_file="$build_dir/xcodebuild.log"
xcode=(
  xcodebuild
  -project "$project"
  -scheme UnitTests
  -configuration "$configuration"
  -destination "platform=macOS,arch=$host_arch"
  -derivedDataPath "$build_dir/DerivedData"
  -parallelizeTargets
  ONLY_ACTIVE_ARCH=YES
  COMPILER_INDEX_STORE_ENABLE=NO
  CODE_SIGNING_ALLOWED=NO
)

if [[ "$requested_configuration" == "ASAN" ]]; then
  xcode+=(-enableAddressSanitizer YES)
elif [[ "$requested_configuration" == "UBSAN" ]]; then
  xcode+=(-enableUndefinedBehaviorSanitizer YES)
fi

settings_file="$build_dir/build-settings.txt"
if ! "${xcode[@]}" -showBuildSettings >"$settings_file" 2>/dev/null; then
  echo "Unable to read build settings for the UnitTests scheme." >&2
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
  echo "No unit-test products were discovered from the UnitTests scheme." >&2
  exit 1
fi

required_test_names=(
  BaseUT
  ConfigUT
  WinCommanderUT
  OperationsUT
  PanelUT
  TermUT
  UtilityUT
  VFSUT
  VFSIconUT
  ViewerUT
)
for required_test_name in "${required_test_names[@]}"; do
  discovered=0
  for test_name in "${test_names[@]}"; do
    if [[ "$test_name" == "$required_test_name" ]]; then
      discovered=1
      break
    fi
  done
  if [[ "$discovered" -ne 1 ]]; then
    echo "Required unit-test product is missing from the UnitTests scheme: $required_test_name" >&2
    exit 1
  fi
done

echo "Building aggregate UnitTests ($requested_configuration, $host_arch)"
"${xcode[@]}" -quiet build 2>&1 | tee "$log_file"

cd "$repo_root"
for index in "${!binary_paths[@]}"; do
  binary_path="${binary_paths[$index]}"
  test_name="${test_names[$index]}"
  if [[ ! -x "$binary_path" ]]; then
    echo "Built test product is not executable: $binary_path" >&2
    exit 1
  fi
  echo "Running $test_name"
  "$binary_path" --rng-seed "$test_seed"
done

echo "All aggregate unit tests passed."
