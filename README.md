![QIDIStudio logo](/resources/images/QIDIStudio.png?raw=true)

# GeekCraft QIDIStudio for Apple Silicon

This is an **unofficial, community-maintained fork** of
[QIDITECH/QIDIStudio](https://github.com/QIDITECH/QIDIStudio). It publishes a
native Apple Silicon build of QIDIStudio and carries a focused patch stack for
QIDI Box synchronization and macOS user-interface reliability.

This project is not affiliated with or supported by QIDI Technology. For the
official product description, documentation, support channels, and upstream
source, see [QIDI's original README](https://github.com/QIDITECH/QIDIStudio#readme).

[Download the latest GeekCraft macOS build](https://github.com/devincorey/QIDIStudio/releases/latest)
· [View the current release notes](https://github.com/devincorey/QIDIStudio/releases/tag/geekcraft-v2.07.02.60-gc.9)
· [Browse the patched source](https://github.com/devincorey/QIDIStudio/tree/geekcraft/02.07.02.60-gc.9)
· [Visit the official QIDIStudio repository](https://github.com/QIDITECH/QIDIStudio)

## Build scope

The downloadable application is a **macOS-only Apple Silicon (`arm64`) build**.
The current `GeekCraft 02.07.02.60-gc.9` release has the following provenance:

| Item | Value |
| --- | --- |
| Upstream release | `QIDIStudio v2.07.02.60` |
| Upstream commit | `c58fdc56629005395a672cb4e8713018a087df1b` |
| Patched source commit | `5365f48848c2c251182dd51e2489a6ca30c9d827` |
| Build system | macOS 27.0 beta (`26A5416b`), macOS SDK 27.0 |
| Compiler | Apple clang 21.0.0 |
| Architecture | Apple Silicon (`arm64`) |
| Deployment target | macOS 12.0 or later |
| Signing | Ad-hoc signed; not Developer ID signed or notarized |

“Built on macOS 27 beta” describes the build environment. It does not mean
macOS 27 is required to run the application.

The default `main` branch tracks upstream source plus this fork landing page.
Patched source for a packaged build lives on its matching `geekcraft/*` release
branch and annotated release tag; use the source link above for `gc.9`.

## What this fork fixes

The current build contains 18 reviewable commits grouped into 14 independently
managed patch sets. Each patch set can be tested, rebased, or retired separately
when QIDI fixes the corresponding behavior upstream.

### 1. QIDI Box synchronization

- Discovers one connected QIDI Box and all four occupied slots instead of
  relying on a synthetic printer object or a fixed external-spool record.
- Uses the selected X-Plus 4 profile as a fallback only when the connected
  printer omits model or nozzle metadata; contradictory reported metadata is
  still rejected.
- Preserves slot order, material identity, and colour while safely handling
  missing colour, remaining-filament, loaded-slot, and external-spool data.
- Keeps every occupied slot mappable even when no Box slot is currently loaded
  into the extruder.
- Makes repeated synchronization deterministic, prevents duplicate slots, and
  retains confirmed mappings across dialog reopenings.
- Exact-matches branded QIDI materials, including QIDI PLA Basic, rather than
  silently substituting PLA Rapido or another product.
- Adds diagnostics for rejected metadata, malformed slots, and preset-resolution
  failures instead of silently returning.
- Cleans up the synchronization dialog, popup, and timers through a single safe
  close path to prevent late callbacks and macOS modal-close crashes.

### 2. Camera fullscreen lifecycle

Prevents the macOS camera viewer from crashing when video is enlarged, closed,
or reopened by fixing the fullscreen mirror and cleanup lifecycle.

### 3. Local build identity

Adds the `GeekCraft <version>` label to the splash screen so the patched build
can be distinguished from an official QIDIStudio installation.

### 4. Box mapping selection outline

Clears the transient selection state after a Box slot is chosen so the popup's
white outline does not remain composited over the application.

### 5. Queued Box popup dismissal

Defers dismissal until the queued macOS slot-selection handler has completed,
then hides the native popup and repaints its owner. This removes the persistent
blank white popup surface left behind after choosing a filament.

### 6. Mapped filament colour refresh

Refreshes both colour surfaces on the send-job mapping card when another Box
slot is selected, including the colour strip at the top of the control. The
project colour is restored when a mapping is removed.

### 7. Auto Bed Leveling option

Carries the send dialog's selected Auto Bed Leveling state into each newly
packaged print job and adds the ten-minute leveling estimate only when leveling
is enabled. Turning the option off no longer packages the full calibration path.

### 8. Signed-bundle diagnostic path

Writes runtime diagnostic results outside the signed application bundle so
normal validation does not invalidate the app's signature.

### 9. macOS toolchain compatibility

Checks whether AppleClang supports the requested warning option before enabling
it, allowing the current Apple toolchain to configure the public-source build.

### 10. Filament catalog fallback

Uses the packaged QIDI filament-index catalog when the X-Plus 4 live catalog
endpoint returns HTTP 404, restoring material and colour resolution without
changing any filament profile values.

### 11. All X-Plus 4 nozzle profiles

Allows missing firmware nozzle metadata to fall back to the selected supported
X-Plus 4 profile at 0.2, 0.4, 0.6, or 0.8 mm. A non-empty, conflicting reported
diameter remains an error.

### 12. Generic Box filament synchronization

Distinguishes Generic Box material identities from branded QIDI preset IDs.
A compatible selected custom preset is retained for Generic material; otherwise
a compatible Generic system preset may be used. Branded QIDI slots still require
an exact product match, material-family mismatches are rejected, and all slots
are resolved before any project mapping is changed.

### 13. Process Advanced switch in Develop mode

Shows the Process Advanced switch as On and disabled whenever Develop mode is
active. Previously the switch could remain visually Off after startup because
the control was disabled before its value was synchronized.

### 14. Cocoa Box popup pre-hide ordering

Explicitly hides the Box mapping popup while wxWidgets still considers it
visible, before clearing and repainting its owner. This closes a macOS-specific
ordering gap that could leave an empty native popup surface composited over the
print dialog after a Box filament was selected.

## What this fork does not change

- It does not modify printer firmware, Klipper, Moonraker, or QIDI Box firmware.
- It does not change QIDI or user filament profiles. Temperatures, flow ratios,
  cooling, speed, retraction, pressure advance, and other process values remain
  exactly as stored in the selected presets.
- It does not change print upload or print-start behavior outside the fixes
  listed above.
- It does not add Intel (`x86_64`), Windows, or Linux release binaries.
- It does not claim to be an official QIDI release.

## Validation

The `gc.9` release passed:

- 344 assertions across 40 focused QIDI Box synchronization test cases;
- the Apple dialog lifecycle suite, including repeated popup selection,
  native pre-hide, dismissal, reopen, owner repaint, cancel, success,
  window-close, and Develop-as-Advanced state paths;
- a full macOS Apple Silicon Release build;
- bundle resource, architecture, dependency, RPATH, and signature checks; and
- an archive round-trip check matching the executable hash and 4,726 packaged
  resources.

The release archive is ad-hoc signed and is not notarized. Its SHA-256 is
published with the GitHub release. The gc.9 Cocoa pre-hide correction has
automated lifecycle and build validation but still requires final observation
on macOS to confirm that the native compositor no longer retains the blank
surface. Existing QIDI Box synchronization completed live X-Plus 4 validation.

## Installing the macOS build

1. Download the ZIP from the
   [latest release](https://github.com/devincorey/QIDIStudio/releases/latest).
2. Optionally verify it with `shasum -a 256 <downloaded-zip>` against the hash
   in that release's checksum file.
3. Extract `QIDIStudio.app` and move it to your Applications folder.
4. Because the app is ad-hoc signed rather than notarized, macOS may require you
   to approve its first launch in **System Settings → Privacy & Security**.

Do not run the official application and this build at the same time against the
same application-data directory. Back up your QIDIStudio configuration before
changing between builds.

## Upstream relationship and maintenance

The patch stack is rebased onto an exact official QIDIStudio release. New
upstream releases are first tested without these patches. A fix is removed only
when its regression is demonstrably fixed upstream; retained patches are
replayed individually and reviewed with source comparison and `git range-diff`.

QIDI's public `v2.07.02.60` source archive does not include the non-public
`QIDINetwork` and `UserPresetSyncManager` sources used by QIDI's internal build.
This fork's public build therefore targets the available direct-LAN QDS path and
does not claim coverage of those unavailable components.

For official QIDIStudio documentation and support, use:

- [QIDI's original QIDIStudio README](https://github.com/QIDITECH/QIDIStudio#readme)
- [QIDIStudio Wiki](https://wiki.qidi3d.com/en/software/qidi-studio)
- [QIDI after-sales support](https://qidi3d.com/pages/warranty-policy-after-sales-support)

## License

QIDIStudio and this fork are licensed under the
[GNU Affero General Public License, version 3](LICENSE).

QIDIStudio is based on Bambu Studio, which is based on PrusaSlicer, which is
based on Slic3r. The upstream project contains the full attribution and license
history in [QIDI's original README](https://github.com/QIDITECH/QIDIStudio#license).
