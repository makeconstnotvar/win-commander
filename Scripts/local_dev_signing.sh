#!/usr/bin/env bash

# Local, stable code-signing support for development builds. A dedicated
# keychain keeps the private key out of login.keychain and is added to the
# user's search list only for the duration of a codesign invocation.

WC_LOCAL_SIGN_IDENTITY="${WINCOMMANDER_LOCAL_SIGN_IDENTITY:-WinCommander Codex Local Code Signing}"
WC_LOCAL_SIGN_KEYCHAIN="${WINCOMMANDER_LOCAL_SIGN_KEYCHAIN:-$HOME/Library/Keychains/WinCommanderCodex.keychain-db}"
WC_LOCAL_SIGN_KEYCHAIN_PASSWORD="${WINCOMMANDER_LOCAL_KEYCHAIN_PASSWORD:-wincommander-codex-local-keychain}"
WC_LOCAL_SIGN_P12_PASSWORD="${WINCOMMANDER_LOCAL_P12_PASSWORD:-wincommander-codex-local-signing}"

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
  local temp_dir key_path cert_path p12_path identity_hash
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

  security import "$p12_path" \
    -k "$WC_LOCAL_SIGN_KEYCHAIN" \
    -P "$WC_LOCAL_SIGN_P12_PASSWORD" \
    -A >/dev/null
  security set-key-partition-list \
    -S apple-tool:,apple:,codesign: \
    -s \
    -k "$WC_LOCAL_SIGN_KEYCHAIN_PASSWORD" \
    "$WC_LOCAL_SIGN_KEYCHAIN" >/dev/null

  identity_hash="$(wc_certificate_hashes | tail -n 1)"
  rm -rf "$temp_dir"

  if [[ -z "$identity_hash" ]] || ! wc_identity_can_sign "$identity_hash"; then
    echo "The local signing certificate was created, but codesign cannot use it." >&2
    return 1
  fi
  printf '%s\n' "$identity_hash"
}

wc_resolve_local_identity() {
  local identity_hash
  wc_ensure_local_keychain
  while IFS= read -r identity_hash; do
    if [[ -n "$identity_hash" ]] && wc_identity_can_sign "$identity_hash"; then
      printf '%s\n' "$identity_hash"
      return 0
    fi
  done < <(wc_certificate_hashes)

  echo "Creating local code-signing identity: $WC_LOCAL_SIGN_IDENTITY" >&2
  wc_create_local_identity
}

wc_designated_requirement() {
  codesign -d -r- "$1" 2>&1 | sed -n '/^designated =>/p'
}

wc_assert_stable_identity() {
  local app_path="$1"
  local requirement
  requirement="$(wc_designated_requirement "$app_path")"

  if [[ -z "$requirement" ]] || grep -q 'designated => cdhash ' <<<"$requirement"; then
    echo "Unstable designated requirement for $app_path" >&2
    echo "  $requirement" >&2
    return 1
  fi
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
