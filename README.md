# EVO-X2 P-MODE Widget

A tiny native Windows desktop widget that displays the current firmware P-MODE on GMKtec EVO-X2 / Sixunited AXB35 systems.

![Application icon](resources/app-icon.png)

## Features

- Quiet / Balanced / Performance status with color-coded text
- 132 × 24 logical pixels at the top-right of the primary work area
- Sits below ordinary application windows (`HWND_BOTTOM`), not always-on-top
- Refreshes every 2.5 seconds
- Notification-area controls for show/hide, refresh, details, and exit
- User-confirmed tray-menu switching between Quiet, Balanced, and Performance on EC firmware 1.08
- Fail-closed display (`P-MODE: --`) on EC contention or invalid telemetry
- Optional highest-privilege per-user logon task for automatic startup
- Native Win32 x64 executable; no web runtime and no console window

## Requirements

- Windows 11 x64
- GMKtec EVO-X2 or another verified Sixunited AXB35 system
- EC firmware 1.04 or newer for status reads; mode switching is restricted to the hardware-verified firmware 1.08
- [official signed PawnIO 2.2.0](https://github.com/namazso/PawnIO.Setup/releases/tag/2.2.0)

Keep Secure Boot enabled and use only the official signed PawnIO edition. Never use the unrestricted/unsigned edition for this widget.

## Installation

1. Download the setup executable and `SHA256SUMS.txt` from the latest [GitHub release](../../releases).
2. Verify the setup SHA-256.
3. Double-click the setup executable and approve the UAC prompt.
4. The installer places the widget under Program Files, creates a Start-menu shortcut, and registers a SID-qualified highest-privilege logon task.

After installation, no PowerShell command is needed. The widget starts immediately and at future Windows logons.

The public beta binaries are not Authenticode-signed, so Windows SmartScreen may warn. Verify both SHA-256 and the GitHub build-provenance attestation:

```powershell
gh attestation verify .\EVO-X2-PMode-Widget-Setup-v0.3.3-beta.3.exe --repo James22bot/evox2-pmode-widget
```

Run setup from the intended local administrator account and approve UAC with that same account. Over-the-shoulder credentials from a different administrator account are not supported in this beta.

A portable ZIP is also published, but because PawnIO normally restricts access to elevated processes, the installer is the recommended path.

## P-MODE map

The persistent EC register map is reverse-engineered:

| EC register `0x31` | Mode |
|---:|---|
| `0x00` | Balanced |
| `0x01` | Performance |
| `0x02` | Quiet |

The map and three-state cycle were verified on a GMKtec NucBox EVO-X2 with EC firmware 1.08.

To change modes, right-click the tray icon and choose **P-MODE umschalten**. Changing to a different mode requires an explicit confirmation whose default answer is **No**. While that dialog is open, nested mode-change requests are rejected; quarantine is checked again after confirmation and immediately before the EC transaction. The widget re-reads the current mode under the EC lock, issues at most one write, waits 200 ms, and accepts success only after two stable target readbacks. If the outcome is unclear, it displays `P-MODE: --`, preserves the first anomaly, and blocks every further write for the rest of that program run. Polling may recover the displayed mode, but switching remains disabled until the hardware state has been established manually and the widget is restarted. It never retries or rolls back automatically.

## Security model

The GUI:

- gates execution on `EVO-X2` or `AXB35` SMBIOS identity;
- verifies the exact PawnIO module size and SHA-256 before loading;
- uses ACPI EC READ command `0x80` for telemetry;
- permits ACPI EC WRITE command `0x81` only after explicit confirmation, only for register `0x31`, only with values `0x00`, `0x01`, or `0x02`, and only on EC firmware 1.08;
- acquires `Global\\Access_EC` for each bounded read and for the complete write-plus-readback transaction;
- revalidates the confirmed starting mode under the lock and performs no automatic write retry or rollback;
- refuses stale EC output and unknown mode values;
- exposes only fixed tray and Explorer-restart messages through UIPI;
- contains no network or persistence API.

The installer is a separate privileged surface. Release artifacts are unsigned, so verify published SHA-256 values and build-provenance attestations before execution. See [SECURITY.md](SECURITY.md).

## Build

Pinned build input: Zig 0.16.0. Host tests use GCC/Clang-compatible C++20 with ASan and UBSan. Windows release artifacts are built in GitHub Actions and packaged with Inno Setup 7.1.0.

```bash
make test
make windows ZIG=/absolute/path/to/zig
```

See [BUILDING.md](BUILDING.md) and the pinned workflow under `.github/workflows/`.

## License

Application source: MIT. The bundled unmodified PawnIO module is LGPL-2.1-or-later; its source and license are under `third_party/`. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
