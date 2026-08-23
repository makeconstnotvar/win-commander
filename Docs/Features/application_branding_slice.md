# Duck Commander application-branding slice

## Result

The product now presents **Duck Commander** across the application, its localizations, network identification, packaging, current documentation, and visual installation material.

The branding contract applies to the MAS, NonMAS, Unsigned, and stable local-development variants. Each variant publishes `Duck Commander` through `CFBundleName` and `CFBundleDisplayName`; MAS and NonMAS also use it as the wrapper name. Established technical identifiers continue to carry local permissions, persisted data, helper admission, source compatibility, and update continuity.

## Display contract

- The application menu, About, Hide, Quit, Help, Preferences, alerts, privacy descriptions, administrator prompts, and English/Russian string catalogs use `Duck Commander`.
- The three application plists publish the current bundle display name and localized Info.plist values.
- The Services declaration publishes `Reveal in Duck Commander`; its `NSPortName` follows the application service port described by [Apple's Services property reference](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/SysServices/Articles/properties.html).
- WebDAV requests use `Duck Commander` as their User-Agent, and SFTP disconnect messages use `Farewell from Duck Commander!`.
- Release, nightly, and unsigned disk images use `duck-commander-*.dmg` names and Duck Commander volume labels.
- The DMG background, installation guide, terminal guide, and current Xcode-project illustration carry the current product or current technical project names.
- Current README, contribution, build, release, help, architecture, design, screenshot-automation, and dependency documentation describes Duck Commander.

## Compatibility identity

The local-development wrapper, bundle identifiers, executable identities, signing identity, designated requirement, Xcode targets and schemes, helper admission names, application-support locations, and installed-client update endpoints retain their established values. This keeps the product-facing brand independent from identifiers that carry system permissions, persisted state, helper trust, source compatibility, and update continuity.

## Regression guard

`LocalizationCatalog_UT.cpp` discovers every `.xcstrings`, `.xib`, and `.storyboard` below `Source/` instead of maintaining a selected file list. It validates:

- all 54 string catalogs and 50 interface resources are scanned for legacy product names;
- the MAS, NonMAS, and Unsigned plists publish exact bundle names;
- English and Russian Info.plist localizations publish `Duck Commander`;
- the previous standalone abbreviation is treated as a product-facing branding token.

The guard currently passes 3/3 cases with 371/371 assertions.

## Verification

- Full Debug `WinCommanderUT --rng-seed 424242`: 928/928 cases and 12,878/12,878 assertions passed.
- Debug unsigned application: built successfully; its processed Info.plist publishes `Duck Commander` through both display-name keys.
- Debug MAS and NonMAS applications: built successfully as `Duck Commander.app`; their processed Info.plists publish the current display name while the compatibility executable remains present.
- Debug `VFSUT --rng-seed 424242`: 211/211 cases and 82,943 assertions passed.
- Debug `PanelUT --rng-seed 424242`: 57/57 cases and 1,385 assertions passed.
- Debug `UtilityUT`: 121/121 cases and 1,776 assertions passed.
- Release ASAN `VFSUT`: 211/211 cases and 82,897 assertions passed with the ASAN runtime linked and a clean diagnostic stream.
- Release UBSAN `VFSUT`: 211/211 cases and 82,919 assertions passed with the UBSAN runtime linked and a clean diagnostic stream.
- All source string catalogs parse as JSON; all three application plists, all application XIB files, build scripts, and screenshot AppleScripts pass their format or syntax checks.
- Two consecutive stable signed no-run rebuilds completed successfully after the final product-resource changes.
- The stable identity verifier passed after the second rebuild with the established bundle, certificate, executable, and designated-requirement values.
- The installed signed app reports `Duck Commander` in its application menu, About dialog, Hide and Quit commands, and Services declaration.
- The installed bundle resource scan contains the current product-facing brand.
