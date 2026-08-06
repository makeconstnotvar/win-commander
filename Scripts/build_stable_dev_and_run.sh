#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DERIVED_DATA_PATH="${WINCOMMANDER_DERIVED_DATA_PATH:-$HOME/Library/Developer/Xcode/DerivedData/WinCommanderCodex}"
INSTALLED_APP_PATH="$HOME/Applications/WinCommander-Codex.app"
ENTITLEMENTS_PATH="$REPOSITORY_ROOT/Source/WinCommander/WinCommander/Resources/WinCommander-CodexDev.entitlements"
DEVELOPER_DIRECTORY="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
DEVELOPMENT_BUNDLE_IDENTIFIER="com.wincommander.App.CodexDev"
MACHINE_LOCK_DIRECTORY="$HOME/Library/Application Support/WinCommanderCodex"
RUN_AFTER_BUILD=1

if [[ "${1:-}" = "--no-run" ]]; then
  RUN_AFTER_BUILD=0
elif [[ -n "${1:-}" ]]; then
  echo "Usage: $0 [--no-run]" >&2
  exit 2
fi

# shellcheck source=local_dev_signing.sh
source "$SCRIPT_DIR/local_dev_signing.sh"

mkdir -p "$MACHINE_LOCK_DIRECTORY"
chmod 700 "$MACHINE_LOCK_DIRECTORY"
exec 9>"$MACHINE_LOCK_DIRECTORY/stable-build.lock"
chmod 600 "$MACHINE_LOCK_DIRECTORY/stable-build.lock"
if ! lockf -s -t 600 9; then
  echo "Timed out waiting for another stable Win Commander build to finish." >&2
  exit 75
fi

mkdir -p "$(dirname "$INSTALLED_APP_PATH")" "$DERIVED_DATA_PATH"
SIGNING_IDENTITY="$(wc_resolve_pinned_local_identity "$INSTALLED_APP_PATH" "$DEVELOPMENT_BUNDLE_IDENTIFIER")"

XCODEBUILD=(
  xcodebuild
  -quiet
  -project "$REPOSITORY_ROOT/Source/WinCommander/WinCommander.xcodeproj"
  -scheme WinCommander-Unsigned
  -configuration Debug
  -derivedDataPath "$DERIVED_DATA_PATH"
  CODE_SIGNING_ALLOWED=NO
  'GCC_PREPROCESSOR_DEFINITIONS=$(inherited) __NC_CODEX_DEV__=1'
)

echo "Building WinCommander development app..."
DEVELOPER_DIR="$DEVELOPER_DIRECTORY" "${XCODEBUILD[@]}" build

SOURCE_APP_PATH="$DERIVED_DATA_PATH/Build/Products/Debug_Unsigned/WinCommander-Unsigned.app"
if [[ ! -d "$SOURCE_APP_PATH" ]]; then
  echo "Build product not found: $SOURCE_APP_PATH" >&2
  exit 1
fi

STAGING_DIRECTORY="$(mktemp -d "${TMPDIR:-/tmp}/wincommander-codex.XXXXXX")"
STAGING_APP_PATH="$STAGING_DIRECTORY/WinCommander-Codex.app"
BACKUP_APP_PATH=""
NEW_APP_INSTALLED=0
INSTALLATION_COMMITTED=0

cleanup() {
  if [[ "$NEW_APP_INSTALLED" = "1" && "$INSTALLATION_COMMITTED" = "0" && -d "$INSTALLED_APP_PATH" ]]; then
    mv "$INSTALLED_APP_PATH" "$STAGING_DIRECTORY/Rejected-WinCommander-Codex.app" 2>/dev/null || true
  fi
  if [[ -n "$BACKUP_APP_PATH" && -d "$BACKUP_APP_PATH" && ! -d "$INSTALLED_APP_PATH" ]]; then
    mv "$BACKUP_APP_PATH" "$INSTALLED_APP_PATH"
  fi
  rm -rf "$STAGING_DIRECTORY"
}
trap cleanup EXIT

ditto "$SOURCE_APP_PATH" "$STAGING_APP_PATH"
/usr/bin/xattr -dr com.apple.quarantine "$STAGING_APP_PATH" 2>/dev/null || true
/usr/bin/xattr -dr com.apple.provenance "$STAGING_APP_PATH" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $DEVELOPMENT_BUNDLE_IDENTIFIER" \
  "$STAGING_APP_PATH/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleName WinCommander Codex Dev" \
  "$STAGING_APP_PATH/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :SUEnableAutomaticChecks bool false" \
  "$STAGING_APP_PATH/Contents/Info.plist" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Set :SUEnableAutomaticChecks false" "$STAGING_APP_PATH/Contents/Info.plist"

# Admin Mode is bound to the upstream Developer ID requirement. Exclude the
# helper from this local build instead of shipping a helper that cannot satisfy
# SMJobBless's mutual requirements.
rm -f "$STAGING_APP_PATH/Contents/Library/LaunchServices/com.wincommander.App.PrivilegedIOHelperV2"
rm -f "$STAGING_APP_PATH/Contents/Library/LaunchServices/com.wincommander.App.CrossVolumeStagingHelperV1"
/usr/libexec/PlistBuddy -c "Delete :SMPrivilegedExecutables" \
  "$STAGING_APP_PATH/Contents/Info.plist" 2>/dev/null || true

wc_sign_app_bundle "$STAGING_APP_PATH" "$SIGNING_IDENTITY" "$ENTITLEMENTS_PATH"
wc_assert_expected_local_identity \
  "$STAGING_APP_PATH" "$DEVELOPMENT_BUNDLE_IDENTIFIER" "$SIGNING_IDENTITY"
wc_adopt_or_assert_pinned_designated_requirement "$STAGING_APP_PATH"
"$SCRIPT_DIR/verify_stable_dev_identity.sh" --candidate "$STAGING_APP_PATH"
NEW_REQUIREMENT="$(wc_designated_requirement "$STAGING_APP_PATH")"

if [[ -d "$INSTALLED_APP_PATH" ]]; then
  OLD_REQUIREMENT="$(wc_designated_requirement "$INSTALLED_APP_PATH")"
  if [[ "$OLD_REQUIREMENT" != "$NEW_REQUIREMENT" ]]; then
    echo "Refusing to replace the development app because its TCC identity changed." >&2
    echo "Existing: $OLD_REQUIREMENT" >&2
    echo "New:      $NEW_REQUIREMENT" >&2
    exit 1
  fi

  while IFS= read -r process_id; do
    [[ -n "$process_id" ]] || continue
    kill "$process_id" 2>/dev/null || true
  done < <(pgrep -f "^$INSTALLED_APP_PATH/Contents/MacOS/" || true)

  BACKUP_APP_PATH="$(mktemp -d "${TMPDIR:-/tmp}/wincommander-codex-backup.XXXXXX")/WinCommander-Codex.app"
  mv "$INSTALLED_APP_PATH" "$BACKUP_APP_PATH"
fi

mv "$STAGING_APP_PATH" "$INSTALLED_APP_PATH"
NEW_APP_INSTALLED=1

"$SCRIPT_DIR/verify_stable_dev_identity.sh"
INSTALLATION_COMMITTED=1

if [[ -n "$BACKUP_APP_PATH" ]]; then
  rm -rf "$(dirname "$BACKUP_APP_PATH")"
  BACKUP_APP_PATH=""
fi

LAUNCH_SERVICES_REGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
"$LAUNCH_SERVICES_REGISTER" -f "$INSTALLED_APP_PATH" >/dev/null 2>&1 || true

echo "Installed: $INSTALLED_APP_PATH"
echo "Bundle ID: $(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$INSTALLED_APP_PATH/Contents/Info.plist")"
echo "Certificate: $SIGNING_IDENTITY"
echo "Requirement: $NEW_REQUIREMENT"

if [[ "$RUN_AFTER_BUILD" = "1" ]]; then
  open "$INSTALLED_APP_PATH"
fi
