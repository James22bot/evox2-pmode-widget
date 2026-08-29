# Reproducible build

## Pinned inputs

| Input | Version/revision | SHA-256 |
|---|---|---|
| Zig Linux x86_64 | 0.16.0 | `70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00` |
| PawnIO.Modules release ZIP | 0.1.6 | `c98af6620d3e7d840bc3c59094bf924ba92cdb9972d50584c814ed69942b248d` |
| `LpcACPIEC.bin` | 0.1.6 | `c38fd116e7aff4d1fdb0a494e296be0a6708e5a22fc72f14587442fb7f8f7906` |

The PawnIO module source is pinned to tag commit `4aa792beb020a14c8261072e9786d2dfb38489d9` and included under `third_party/` with its LGPL-2.1-or-later license.

## Host tests

```bash
make test
```

This compiles both test binaries with GCC/G++ and `-Wall -Wextra -Wpedantic -Werror`, AddressSanitizer and UndefinedBehaviorSanitizer.

## Windows x64 GUI build

```bash
make windows ZIG=/absolute/path/to/zig
```

The build:

- verifies the exact PawnIO module hash before linking;
- compiles the application manifest and version resource with `zig rc`;
- links a Windows GUI-subsystem PE32+ binary;
- embeds a `requireAdministrator` UAC manifest and PerMonitorV2 DPI declaration;
- enables stack protection and strips debug metadata;
- does not embed `LpcACPIEC.bin`; the signed module stays a separately replaceable LGPL component and is verified at runtime.

Output:

```text
build/windows/evox2-pmode-overlay.exe
```

On Windows, use the pinned build script:

```powershell
.\scripts\build-windows.ps1 -Zig C:\path\to\zig.exe
```

## Installer and release package

The release workflow pins:

- Zig 0.16.0 Windows archive: `68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e`
- Inno Setup 7.1.0 x64 installer: `0362a383ed217d4c4239b5933866dd96d3eb2102737da92f80f6057a4b40df2f`

After building the widget:

```powershell
.\scripts\package-release.ps1 -Iscc C:\path\to\ISCC.exe
```

This produces a setup executable, portable ZIP, and `SHA256SUMS.txt` under `dist/`. GitHub Actions then performs a silent install/uninstall smoke test before any tagged prerelease is published.

## Scope

The build host cannot execute the GUI or PawnIO path. Unit tests, static PE checks and reproducibility are build-host evidence; the actual overlay appearance and live mode refresh require the target EVO-X2 desktop.
