#!/usr/bin/env bash

# Local, stable code-signing support for development builds. A dedicated
# keychain keeps the private key out of login.keychain and is added to the
# user's search list only for the duration of a codesign invocation.

WC_LOCAL_SIGN_IDENTITY="${WINCOMMANDER_LOCAL_SIGN_IDENTITY:-WinCommander Codex Local Code Signing}"
WC_LOCAL_SIGN_KEYCHAIN="${WINCOMMANDER_LOCAL_SIGN_KEYCHAIN:-$HOME/Library/Keychains/WinCommanderCodex.keychain-db}"
WC_LOCAL_SIGN_KEYCHAIN_PASSWORD="${WINCOMMANDER_LOCAL_KEYCHAIN_PASSWORD:-wincommander-codex-local-keychain}"
WC_LOCAL_SIGN_P12_PASSWORD="${WINCOMMANDER_LOCAL_P12_PASSWORD:-wincommander-codex-local-signing}"
WC_LOCAL_SIGN_STATE_DIR="${WINCOMMANDER_LOCAL_SIGN_STATE_DIR:-$HOME/Library/Application Support/WinCommanderCodex}"
WC_LOCAL_SIGN_IDENTITY_PIN="${WINCOMMANDER_LOCAL_SIGN_IDENTITY_PIN:-$WC_LOCAL_SIGN_STATE_DIR/local-signing-identity.sha1}"
WC_LOCAL_SIGN_REQUIREMENT_PIN="${WINCOMMANDER_LOCAL_SIGN_REQUIREMENT_PIN:-$WC_LOCAL_SIGN_STATE_DIR/local-signing-requirement.txt}"

wc_user_keychains() {
  security list-keychains -d user 2>/dev/null \
    | tr -d '"' \
    | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' \
    | sed '/^$/d'
}

wc_add_keychain_to_search_list() {
  local keychain="$1"
  local current_keychains
  current_keychains="$(wc_user_keychains)"

  grep -Fxq "$keychain" <<<"$current_keychains" && return 0
  security list-keychains -d user -s "$keychain" $current_keychains >/dev/null
}

wc_remove_keychain_from_search_list() {
  local keychain="$1"
  local current_keychains remaining
  current_keychains="$(wc_user_keychains)"

  grep -Fxq "$keychain" <<<"$current_keychains" || return 0
  remaining="$(grep -Fxv "$keychain" <<<"$current_keychains")"
  [[ -n "$remaining" ]] || return 0
  security list-keychains -d user -s $remaining >/dev/null
}

wc_ensure_local_keychain() {
  mkdir -p "$(dirname "$WC_LOCAL_SIGN_KEYCHAIN")"
  if [[ ! -f "$WC_LOCAL_SIGN_KEYCHAIN" ]]; then
    security create-keychain -p "$WC_LOCAL_SIGN_KEYCHAIN_PASSWORD" "$WC_LOCAL_SIGN_KEYCHAIN" >/dev/null
  fi
  chmod 600 "$WC_LOCAL_SIGN_KEYCHAIN"

  security unlock-keychain -p "$WC_LOCAL_SIGN_KEYCHAIN_PASSWORD" "$WC_LOCAL_SIGN_KEYCHAIN" >/dev/null
  security set-keychain-settings -lut 21600 "$WC_LOCAL_SIGN_KEYCHAIN" >/dev/null
  wc_remove_keychain_from_search_list "$WC_LOCAL_SIGN_KEYCHAIN"
}

wc_certificate_hashes() {
  security find-certificate -a -c "$WC_LOCAL_SIGN_IDENTITY" -Z "$WC_LOCAL_SIGN_KEYCHAIN" 2>/dev/null \
    | sed -n 's/^[[:space:]]*SHA-1 hash: //p'
}

wc_codesign() {
  local identity="$1"
  local status=0
  shift

  wc_add_keychain_to_search_list "$WC_LOCAL_SIGN_KEYCHAIN"
  codesign --keychain "$WC_LOCAL_SIGN_KEYCHAIN" --sign "$identity" "$@" || status=$?
  wc_remove_keychain_from_search_list "$WC_LOCAL_SIGN_KEYCHAIN"
  return "$status"
}

wc_identity_can_sign() {
  local identity="$1"
  local probe_dir probe_pid status_file status
  probe_dir="$(mktemp -d)"
  status_file="$probe_dir/status"
  cp /usr/bin/true "$probe_dir/probe"

  (
    set +e
    wc_codesign "$identity" --force --timestamp=none "$probe_dir/probe" >/dev/null 2>&1
    printf '%s\n' "$?" >"$status_file"
  ) &
  probe_pid=$!

  for _ in {1..50}; do
    if [[ -f "$status_file" ]]; then
      status="$(cat "$status_file")"
      wait "$probe_pid" >/dev/null 2>&1 || true
      rm -rf "$probe_dir"
      [[ "$status" = "0" ]]
      return $?
    fi
    sleep 0.1
  done

  kill "$probe_pid" >/dev/null 2>&1 || true
  kill -9 "$probe_pid" >/dev/null 2>&1 || true
  wait "$probe_pid" >/dev/null 2>&1 || true
  rm -rf "$probe_dir"
  return 1
}

wc_create_local_identity() {
  local temp_dir key_path cert_path p12_path identity_hash imported_identity_hash
  command -v openssl >/dev/null 2>&1 || {
    echo "openssl is required to create the local signing identity." >&2
    return 1
  }

  wc_ensure_local_keychain
  temp_dir="$(mktemp -d)"
  key_path="$temp_dir/wincommander-local.key"
  cert_path="$temp_dir/wincommander-local.crt"
  p12_path="$temp_dir/wincommander-local.p12"

  openssl req \
    -new \
    -newkey rsa:2048 \
    -x509 \
    -days 3650 \
    -nodes \
    -subj "/CN=$WC_LOCAL_SIGN_IDENTITY/" \
    -addext "keyUsage=critical,digitalSignature" \
    -addext "extendedKeyUsage=codeSigning" \
    -keyout "$key_path" \
    -out "$cert_path" >/dev/null 2>&1

  openssl pkcs12 \
    -export \
    -inkey "$key_path" \
    -in "$cert_path" \
    -out "$p12_path" \
    -name "$WC_LOCAL_SIGN_IDENTITY" \
    -passout "pass:$WC_LOCAL_SIGN_P12_PASSWORD" >/dev/null 2>&1

  identity_hash="$(openssl x509 -in "$cert_path" -noout -fingerprint -sha1 \
    | sed 's/.*=//' | tr -d ':' | tr '[:lower:]' '[:upper:]')"
  if [[ ${#identity_hash} -ne 40 ]]; then
    rm -rf "$temp_dir"
    echo "Could not determine the new local signing certificate fingerprint." >&2
    return 1
  fi

  security import "$p12_path" \
    -k "$WC_LOCAL_SIGN_KEYCHAIN" \
    -P "$WC_LOCAL_SIGN_P12_PASSWORD" \
    -x \
    -T /usr/bin/codesign >/dev/null
  security set-key-partition-list \
    -S apple-tool:,apple:,codesign: \
    -s \
    -k "$WC_LOCAL_SIGN_KEYCHAIN_PASSWORD" \
    "$WC_LOCAL_SIGN_KEYCHAIN" >/dev/null

  imported_identity_hash="$(wc_certificate_hashes \
    | tr '[:lower:]' '[:upper:]' \
    | grep -Fx "$identity_hash" || true)"
  rm -rf "$temp_dir"

  if [[ "$imported_identity_hash" != "$identity_hash" ]] || ! wc_identity_can_sign "$identity_hash"; then
    echo "The local signing certificate was created, but codesign cannot use it." >&2
    return 1
  fi
  printf '%s\n' "$identity_hash"
}

wc_normalize_certificate_hash() {
  printf '%s' "$1" | tr '[:lower:]' '[:upper:]'
}

wc_assert_private_state_file() {
  local path="$1"
  local mode owner current_user

  if [[ ! -f "$path" || -L "$path" ]]; then
    echo "Local signing state must be a regular, non-symlink file: $path" >&2
    return 1
  fi
  mode="$(stat -f '%Lp' "$path")"
  owner="$(stat -f '%Su' "$path")"
  current_user="$(id -un)"
  if [[ "$mode" != "600" || "$owner" != "$current_user" ]]; then
    echo "Unsafe local signing state permissions: $path (owner=$owner mode=$mode)" >&2
    return 1
  fi
}

wc_read_identity_pin() {
  local identity_hash

  [[ -e "$WC_LOCAL_SIGN_IDENTITY_PIN" || -L "$WC_LOCAL_SIGN_IDENTITY_PIN" ]] || return 1
  wc_assert_private_state_file "$WC_LOCAL_SIGN_IDENTITY_PIN" || return 2
  identity_hash="$(tr -d '\r\n' <"$WC_LOCAL_SIGN_IDENTITY_PIN")"
  if [[ ${#identity_hash} -ne 40 ]] || [[ "$identity_hash" = *[!0-9A-Fa-f]* ]]; then
    echo "Invalid local signing identity pin: $WC_LOCAL_SIGN_IDENTITY_PIN" >&2
    return 2
  fi
  wc_normalize_certificate_hash "$identity_hash"
}

wc_write_identity_pin() {
  local identity_hash="$1"
  local temporary_pin

  identity_hash="$(wc_normalize_certificate_hash "$identity_hash")"
  mkdir -p "$WC_LOCAL_SIGN_STATE_DIR"
  chmod 700 "$WC_LOCAL_SIGN_STATE_DIR"
  temporary_pin="$(mktemp "$WC_LOCAL_SIGN_STATE_DIR/.local-signing-identity.XXXXXX")"
  chmod 600 "$temporary_pin"
  printf '%s\n' "$identity_hash" >"$temporary_pin"
  mv "$temporary_pin" "$WC_LOCAL_SIGN_IDENTITY_PIN"
}

wc_read_requirement_pin() {
  local requirement

  [[ -e "$WC_LOCAL_SIGN_REQUIREMENT_PIN" || -L "$WC_LOCAL_SIGN_REQUIREMENT_PIN" ]] || return 1
  wc_assert_private_state_file "$WC_LOCAL_SIGN_REQUIREMENT_PIN" || return 2
  requirement="$(cat "$WC_LOCAL_SIGN_REQUIREMENT_PIN")"
  if [[ -z "$requirement" ]] || [[ "$requirement" != 'designated => '* ]] || [[ "$requirement" = *$'\n'* ]]; then
    echo "Invalid local signing requirement pin: $WC_LOCAL_SIGN_REQUIREMENT_PIN" >&2
    return 2
  fi
  printf '%s\n' "$requirement"
}

wc_write_requirement_pin() {
  local requirement="$1"
  local temporary_pin

  mkdir -p "$WC_LOCAL_SIGN_STATE_DIR"
  chmod 700 "$WC_LOCAL_SIGN_STATE_DIR"
  temporary_pin="$(mktemp "$WC_LOCAL_SIGN_STATE_DIR/.local-signing-requirement.XXXXXX")"
  chmod 600 "$temporary_pin"
  printf '%s\n' "$requirement" >"$temporary_pin"
  mv "$temporary_pin" "$WC_LOCAL_SIGN_REQUIREMENT_PIN"
}

wc_resolve_local_identity() {
  local required_identity_hash="${1:-}"
  local identity_hash normalized_identity_hash
  local -a usable_identity_hashes=()

  if [[ -n "$required_identity_hash" ]]; then
    required_identity_hash="$(wc_normalize_certificate_hash "$required_identity_hash")"
    if [[ ! -f "$WC_LOCAL_SIGN_KEYCHAIN" ]]; then
      echo "The pinned local signing keychain is missing: $WC_LOCAL_SIGN_KEYCHAIN" >&2
      echo "Refusing to create a replacement identity because that would invalidate existing TCC grants." >&2
      return 1
    fi
  fi

  wc_ensure_local_keychain
  while IFS= read -r identity_hash; do
    [[ -n "$identity_hash" ]] || continue
    normalized_identity_hash="$(wc_normalize_certificate_hash "$identity_hash")"
    [[ -z "$required_identity_hash" || "$normalized_identity_hash" = "$required_identity_hash" ]] || continue
    wc_identity_can_sign "$identity_hash" || continue
    usable_identity_hashes+=("$normalized_identity_hash")
  done < <(wc_certificate_hashes)

  if [[ ${#usable_identity_hashes[@]} -eq 1 ]]; then
    printf '%s\n' "${usable_identity_hashes[0]}"
    return 0
  fi

  if [[ ${#usable_identity_hashes[@]} -gt 1 ]]; then
    echo "Multiple usable local signing certificates match $WC_LOCAL_SIGN_IDENTITY." >&2
    echo "Refusing to choose one implicitly because that could change the TCC identity." >&2
    return 1
  fi

  if [[ -n "$required_identity_hash" ]]; then
    echo "Pinned local signing certificate $required_identity_hash is unavailable." >&2
    echo "Refusing to create a replacement identity because that would invalidate existing TCC grants." >&2
    return 1
  fi

  echo "Creating local code-signing identity: $WC_LOCAL_SIGN_IDENTITY" >&2
  wc_create_local_identity
}

wc_designated_requirement() {
  codesign -d -r- "$1" 2>&1 | sed -n '/^designated =>/p'
}

wc_requirement_certificate_hash() {
  local requirement
  requirement="$(wc_designated_requirement "$1")"
  sed -n 's/.*certificate leaf = H"\([0-9A-Fa-f]*\)".*/\1/p' <<<"$requirement" \
    | tr '[:lower:]' '[:upper:]'
}

wc_assert_stable_identity() {
  local app_path="$1"
  local requirement
  requirement="$(wc_designated_requirement "$app_path")"

  if [[ -z "$requirement" ]] || grep -q 'cdhash' <<<"$requirement"; then
    echo "Unstable designated requirement for $app_path" >&2
    echo "  $requirement" >&2
    return 1
  fi
}

wc_assert_expected_local_identity() {
  local app_path="$1"
  local expected_bundle_identifier="$2"
  local expected_certificate_hash="${3:-}"
  local actual_bundle_identifier actual_certificate_hash

  codesign --verify --deep --strict "$app_path"
  actual_bundle_identifier="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' \
    "$app_path/Contents/Info.plist" 2>/dev/null || true)"
  if [[ "$actual_bundle_identifier" != "$expected_bundle_identifier" ]]; then
    echo "Unexpected bundle identifier for $app_path" >&2
    echo "Expected: $expected_bundle_identifier" >&2
    echo "Actual:   $actual_bundle_identifier" >&2
    return 1
  fi

  wc_assert_stable_identity "$app_path"
  if ! grep -Fq "identifier \"$expected_bundle_identifier\"" \
    <<<"$(wc_designated_requirement "$app_path")"; then
    echo "The designated requirement does not bind the expected bundle identifier for $app_path" >&2
    return 1
  fi
  actual_certificate_hash="$(wc_requirement_certificate_hash "$app_path")"
  if [[ ${#actual_certificate_hash} -ne 40 ]]; then
    echo "The designated requirement does not contain a stable certificate leaf hash for $app_path" >&2
    return 1
  fi

  if [[ -n "$expected_certificate_hash" ]]; then
    expected_certificate_hash="$(wc_normalize_certificate_hash "$expected_certificate_hash")"
    if [[ "$actual_certificate_hash" != "$expected_certificate_hash" ]]; then
      echo "Unexpected signing certificate for $app_path" >&2
      echo "Expected: $expected_certificate_hash" >&2
      echo "Actual:   $actual_certificate_hash" >&2
      return 1
    fi
  fi
}

wc_assert_pinned_designated_requirement() {
  local app_path="$1"
  local pinned_requirement actual_requirement

  if ! pinned_requirement="$(wc_read_requirement_pin)"; then
    echo "Stable development requirement pin not found: $WC_LOCAL_SIGN_REQUIREMENT_PIN" >&2
    return 1
  fi
  actual_requirement="$(wc_designated_requirement "$app_path")"
  if [[ "$actual_requirement" != "$pinned_requirement" ]]; then
    echo "The app does not match the pinned designated requirement: $app_path" >&2
    echo "Pinned: $pinned_requirement" >&2
    echo "Actual: $actual_requirement" >&2
    return 1
  fi
}

wc_adopt_or_assert_pinned_designated_requirement() {
  local app_path="$1"
  local pinned_requirement="" actual_requirement read_status

  actual_requirement="$(wc_designated_requirement "$app_path")"
  if pinned_requirement="$(wc_read_requirement_pin)"; then
    :
  else
    read_status=$?
    if [[ "$read_status" -ne 1 ]]; then
      return 1
    fi
    wc_write_requirement_pin "$actual_requirement"
    pinned_requirement="$actual_requirement"
  fi

  if [[ "$actual_requirement" != "$pinned_requirement" ]]; then
    echo "Refusing designated-requirement drift for $app_path" >&2
    echo "Pinned: $pinned_requirement" >&2
    echo "Actual: $actual_requirement" >&2
    return 1
  fi
}

wc_resolve_pinned_local_identity() {
  local installed_app_path="$1"
  local expected_bundle_identifier="$2"
  local pinned_identity_hash="" installed_identity_hash="" resolved_identity_hash

  if pinned_identity_hash="$(wc_read_identity_pin)"; then
    :
  else
    case "$?" in
      1) pinned_identity_hash="" ;;
      *) return 1 ;;
    esac
  fi

  if [[ -d "$installed_app_path" ]]; then
    wc_assert_expected_local_identity "$installed_app_path" "$expected_bundle_identifier"
    installed_identity_hash="$(wc_requirement_certificate_hash "$installed_app_path")"
    if [[ -n "$pinned_identity_hash" && "$installed_identity_hash" != "$pinned_identity_hash" ]]; then
      echo "The installed development app does not match the pinned local signing identity." >&2
      echo "Installed: $installed_identity_hash" >&2
      echo "Pinned:    $pinned_identity_hash" >&2
      return 1
    fi
    if [[ -z "$pinned_identity_hash" ]]; then
      pinned_identity_hash="$installed_identity_hash"
      wc_write_identity_pin "$pinned_identity_hash"
    fi
    wc_adopt_or_assert_pinned_designated_requirement "$installed_app_path"
  fi

  resolved_identity_hash="$(wc_resolve_local_identity "$pinned_identity_hash")"
  if [[ -z "$pinned_identity_hash" ]]; then
    wc_write_identity_pin "$resolved_identity_hash"
  fi
  printf '%s\n' "$resolved_identity_hash"
}

wc_sign_code_object() {
  local code_object="$1"
  local identity="$2"
  shift 2

  [[ -e "$code_object" ]] || return 0
  wc_codesign "$identity" \
    --force \
    --options runtime \
    --timestamp=none \
    "$@" \
    "$code_object" >/dev/null
}

wc_sign_app_bundle() {
  local app_path="$1"
  local identity="$2"
  local entitlements_path="$3"
  local sparkle_root="$app_path/Contents/Frameworks/Sparkle.framework"

  # Sign the deepest code first. Component identifiers and entitlements must
  # remain explicit, so --deep is reserved for verification below.
  wc_sign_code_object \
    "$sparkle_root/Versions/Current/XPCServices/Downloader.xpc" "$identity"
  wc_sign_code_object \
    "$sparkle_root/Versions/Current/XPCServices/Installer.xpc" "$identity"
  wc_sign_code_object \
    "$sparkle_root/Versions/Current/Updater.app" "$identity"
  wc_sign_code_object \
    "$sparkle_root/Versions/Current/Autoupdate" "$identity" \
    --identifier org.sparkle-project.Sparkle.Autoupdate
  wc_sign_code_object "$sparkle_root" "$identity"

  wc_sign_code_object "$app_path/Contents/Frameworks/LetsMove.framework" "$identity"
  wc_sign_code_object \
    "$app_path/Contents/XPCServices/Highlighter.xpc" "$identity" \
    --entitlements "$(dirname "$entitlements_path")/../../../Viewer/resources/Highlighter.entitlements"
  wc_sign_code_object "$app_path/Contents/MacOS/WinCommander-Unsigned.debug.dylib" "$identity"
  wc_sign_code_object "$app_path/Contents/MacOS/__preview.dylib" "$identity"

  wc_codesign "$identity" \
    --force \
    --options runtime \
    --timestamp=none \
    --entitlements "$entitlements_path" \
    "$app_path" >/dev/null

  codesign --verify --deep --strict --verbose=2 "$app_path"
  wc_assert_stable_identity "$app_path"
}
