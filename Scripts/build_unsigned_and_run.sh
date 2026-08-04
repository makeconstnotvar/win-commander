#!/bin/sh
set -eu
set -o pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT="$SCRIPT_DIR/../Source/WinCommander/WinCommander.xcodeproj"

build() {
    xcodebuild -project "$PROJECT" -scheme WinCommander-Unsigned -configuration Debug "$@"
}

APP_DIR=$(build -showBuildSettings | sed -n 's/^ *BUILT_PRODUCTS_DIR = //p')
APP_NAME=$(build -showBuildSettings | sed -n 's/^ *FULL_PRODUCT_NAME = //p')
APP_PATH="$APP_DIR/$APP_NAME"

build build
open "$APP_PATH"
