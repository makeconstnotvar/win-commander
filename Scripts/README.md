# Helper Scripts
The `Scripts` directory contains a set of useful scripts to be used during development, continuous integration, testing and packaging.

## `verify_m0.sh`
Runs the M0 baseline in one command: requires Xcode 26.5, validates the Xcode project, builds `WinCommander-Unsigned` in the Debug configuration without signing, then builds and executes every product in the aggregate `UnitTests` scheme. Discovery must include the ten baseline products (`BaseUT`, `ConfigUT`, `WinCommanderUT`, `OperationsUT`, `PanelUT`, `TermUT`, `UtilityUT`, `VFSUT`, `VFSIconUT`, and `ViewerUT`); additional products are also executed.

Run it from any directory:

```sh
/path/to/win-commander/Scripts/verify_m0.sh
```

Only `xcodebuild` is required. Temporary build artifacts are removed after the run. Set `WINCOMMANDER_KEEP_BUILD_ARTIFACTS=1` to retain them, or set `WINCOMMANDER_M0_BUILD_DIR` to choose an explicit artifact directory. GitHub Actions runs this same entrypoint from `.github/workflows/m0-verification.yml` on macOS 26 with Xcode 26.5 and does not use signing credentials.
Catch2 binaries use the reproducible seed `424242`; set `WINCOMMANDER_TEST_SEED` to override it for exploratory runs.

## `build_for_codeql.sh`
Builds tests and the main application without running so that CodeQL can intercept the commands and perform its analysis afterwards.  
`xcodebuild` and `xcpretty` must be available in the environment in order for this script to run.  

## `build_help.sh`
Converts the markdown documention into a pdf placed in `build_help.tmp/Help.pdf`

## `build_mas_archive.sh`
Builds and archive Win Commander for submission to MacAppStore.

## `build_nightly.sh`
Builds Win Commander with the `WinCommander-NonMAS` scheme / `Release` configuration, signs it, packages the runnable build into a `.dmg` image and notarizes the final image.  
`xcodebuild`, `xcpretty` and `create-dmg` must be available in the environment in order for this script to run.  
It also requires the codesigning certificate to be properly signed.  

## `build_release.sh`  
Same a `build_nightly.sh`, but creates a release build. 

## `build_unsigned.sh`
Builds Win Commander with the `WinCommander-Unsigned` scheme / `Release` configuration and packages the runnable build into a `.dmg` image.  
`xcodebuild`, `xcpretty` and `create-dmg` must be available in the environment in order for this script to run.  

## `build_unsigned_and_run.sh`
Builds Win Commander with the `WinCommander-Unsigned` scheme / `Debug` configuration and runs it afterwards.  
`xcodebuild` must be available in the environment in order for this script to run.

## `build_stable_dev_and_run.sh [--no-run]`
Builds the Debug application, signs it with a persistent local development identity, installs it at
`~/Applications/WinCommander-Codex.app`, and runs that stable copy. This preserves the macOS TCC identity across
rebuilds, so filesystem and automation permissions normally need to be granted only once for this identity. The
grant is needed again only after a TCC reset or an intentional certificate replacement. The signing key is kept in a
dedicated keychain that is exposed to `codesign` only while signing. Pass `--no-run` to build, sign, and install
without launching the application. This local build disables Sparkle/LetsMove and excludes Admin Mode because the
privileged helper is intentionally bound to the upstream Developer ID certificate. Library validation alone is
disabled for this development bundle because a self-signed certificate has no Apple Team ID for the Debug dylibs.

## `run_all_integration_tests.sh`
Builds and executes the aggregate integration suite with Debug ASAN instrumentation. The script starts local FTP,
SFTP and WebDAV Docker fixtures, waits for their ports, builds in an isolated temporary DerivedData directory, runs
each discovered integration-test product, and removes fixture containers and images on every exit path.

`xcodebuild`, a running Docker daemon and `nc` are required. `xcpretty` is optional. The script can be called from
any directory. Set `WINCOMMANDER_KEEP_BUILD_ARTIFACTS=1` to retain its temporary build directory, or set
`WINCOMMANDER_INTEGRATION_TEST_BUILD_DIR` to choose an explicit directory. Catch2 uses seed `424242` unless
`WINCOMMANDER_TEST_SEED` is set.

## `run_all_unit_tests.sh [Debug|Release|ASAN|UBSAN]`
Builds and executes all unit tests with the specified configuration.  
Only `xcodebuild` must be available. The script can be called from any directory and removes its temporary build directory after the run. Set `WINCOMMANDER_KEEP_BUILD_ARTIFACTS=1` to retain it, or set `WINCOMMANDER_UNIT_TEST_BUILD_DIR` to choose an explicit directory.
Catch2 uses seed `424242` unless `WINCOMMANDER_TEST_SEED` is set.

## `run_clang_format.sh`
Executes `clang-format` against all source files in the `Source` directory, re-formatting them in-place if necessary.  
Rules from `Source/.clang-format` are used in the process.  
`clang-format` must be available in order for this script to run.

## `run_clang_tidy.sh`
Executes `clang-tidy` against all source files in the `Source` directory, updating them in-place if necessary.  
`xcodebuild`, `xcpretty` and `jq` must be available in the environment in order for this script to run.  
`clang-tidy` must be installed via Brew and is expected to be located at `/opt/homebrew/opt/llvm/bin/`.  
Rules from `Source/.clang-tidy` are used in the process.  
It's recommended to execute `run_clang_format.sh` afterwards.

## Dependencies installation:
  * xcodebuild:
    * Install Xcode 26.5 for the M0 baseline.
  * [xcpretty](https://github.com/xcpretty/xcpretty): `gem install xcpretty`
  * [clang-format](https://clang.llvm.org/docs/ClangFormat.html): `brew install clang-format`
  * [clang-tidy](https://clang.llvm.org/extra/clang-tidy/): `brew install llvm`
  * [jq](https://jqlang.github.io/jq/): `brew install jq`
  * [create-dmg](https://github.com/create-dmg/create-dmg): `brew install create-dmg`
