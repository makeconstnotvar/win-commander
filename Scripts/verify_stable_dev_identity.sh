#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_APP_PATH="$HOME/Applications/WinCommander-Codex.app"
EXPECTED_BUNDLE_IDENTIFIER="com.wincommander.App.CodexDev"
EXPECTED_BUNDLE_NAME="Duck Commander"
EXPECTED_BUNDLE_DISPLAY_NAME="Duck Commander"
EXPECTED_EXECUTABLE_NAME="WinCommander-Unsigned"
EXPECTED_ENTITLEMENTS_PATH="$SCRIPT_DIR/../Source/WinCommander/WinCommander/Resources/WinCommander-CodexDev.entitlements"
REQUIRE_CANONICAL_PATH=1

case "${1:-}" in
  "")
    APP_PATH="$DEFAULT_APP_PATH"
    ;;
  --candidate)
    if [[ -z "${2:-}" || -n "${3:-}" ]]; then
      echo "Usage: $0 [--candidate app-path]" >&2
      exit 2
    fi
    APP_PATH="$2"
    REQUIRE_CANONICAL_PATH=0
    ;;
  *)
    echo "Usage: $0 [--candidate app-path]" >&2
    exit 2
    ;;
esac

# shellcheck source=local_dev_signing.sh
source "$SCRIPT_DIR/local_dev_signing.sh"

STATE_DIR_MODE="$(stat -f '%Lp' "$WC_LOCAL_SIGN_STATE_DIR" 2>/dev/null || true)"
STATE_DIR_OWNER="$(stat -f '%Su' "$WC_LOCAL_SIGN_STATE_DIR" 2>/dev/null || true)"
CURRENT_USER="$(id -un)"
if [[ -L "$WC_LOCAL_SIGN_STATE_DIR" || "$STATE_DIR_MODE" != "700" || "$STATE_DIR_OWNER" != "$CURRENT_USER" ]]; then
  echo "Unsafe stable development state directory: $WC_LOCAL_SIGN_STATE_DIR" >&2
  exit 1
fi
wc_assert_private_state_file "$WC_LOCAL_SIGN_IDENTITY_PIN"
wc_assert_private_state_file "$WC_LOCAL_SIGN_REQUIREMENT_PIN"
wc_assert_private_state_file "$WC_LOCAL_SIGN_KEYCHAIN"

if [[ ! -d "$APP_PATH" ]]; then
  echo "Stable development app not found: $APP_PATH" >&2
  exit 1
fi

if [[ "$REQUIRE_CANONICAL_PATH" = "1" && "$APP_PATH" != "$DEFAULT_APP_PATH" ]]; then
  echo "Unexpected stable development app path: $APP_PATH" >&2
  exit 1
fi

if ! PINNED_IDENTITY="$(wc_read_identity_pin)"; then
  echo "Stable development identity pin not found: $WC_LOCAL_SIGN_IDENTITY_PIN" >&2
  exit 1
fi

wc_assert_expected_local_identity \
  "$APP_PATH" "$EXPECTED_BUNDLE_IDENTIFIER" "$PINNED_IDENTITY"
wc_assert_pinned_designated_requirement "$APP_PATH"
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

INFO_PLIST="$APP_PATH/Contents/Info.plist"
for key_and_value in \
  "CFBundleName=$EXPECTED_BUNDLE_NAME" \
  "CFBundleDisplayName=$EXPECTED_BUNDLE_DISPLAY_NAME" \
  "CFBundleExecutable=$EXPECTED_EXECUTABLE_NAME"; do
  key="${key_and_value%%=*}"
  expected="${key_and_value#*=}"
  actual="$(/usr/libexec/PlistBuddy -c "Print :$key" "$INFO_PLIST" 2>/dev/null || true)"
  if [[ "$actual" != "$expected" ]]; then
    echo "Unexpected $key in the stable local build: expected '$expected', got '$actual'." >&2
    exit 1
  fi
done

ACTUAL_ENTITLEMENTS="$(mktemp "${TMPDIR:-/tmp}/wincommander-entitlements.XXXXXX")"
cleanup() {
  rm -f "$ACTUAL_ENTITLEMENTS"
}
trap cleanup EXIT
codesign -d --entitlements - --xml "$APP_PATH" 2>&1 | sed -n '/^<?xml /,$p' >"$ACTUAL_ENTITLEMENTS"
if [[ "$(plutil -convert json -o - "$ACTUAL_ENTITLEMENTS")" != \
  "$(plutil -convert json -o - "$EXPECTED_ENTITLEMENTS_PATH")" ]]; then
  echo "Unexpected entitlements for the stable local build: $APP_PATH" >&2
  exit 1
fi

if /usr/libexec/PlistBuddy -c 'Print :SMPrivilegedExecutables' \
  "$APP_PATH/Contents/Info.plist" >/dev/null 2>&1; then
  echo "Unexpected SMPrivilegedExecutables declaration in the stable local build." >&2
  exit 1
fi

for helper in \
  com.wincommander.App.PrivilegedIOHelperV2 \
  com.wincommander.App.CrossVolumeStagingHelperV1; do
  if [[ -e "$APP_PATH/Contents/Library/LaunchServices/$helper" ]]; then
    echo "Unexpected privileged helper in the stable local build: $helper" >&2
    exit 1
  fi
done

echo "Stable development identity verified."
echo "App:         $APP_PATH"
echo "Bundle ID:   $EXPECTED_BUNDLE_IDENTIFIER"
echo "Certificate: $PINNED_IDENTITY"
echo "Requirement: $(wc_designated_requirement "$APP_PATH")"
