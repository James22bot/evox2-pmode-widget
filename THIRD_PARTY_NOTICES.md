# Third-party notices

## PawnIO Modules — LpcACPIEC

This distribution includes the unmodified files:

- `third_party/PawnIO.Modules-0.1.6/LpcACPIEC.bin`
- `third_party/PawnIO.Modules-0.1.6/LpcACPIEC.p`
- `third_party/PawnIO.Modules-0.1.6/COPYING`

Upstream: <https://github.com/namazso/PawnIO.Modules>
Release: `0.1.6`
Source commit: `4aa792beb020a14c8261072e9786d2dfb38489d9`
License: LGPL-2.1-or-later

Module SHA-256:

```text
c38fd116e7aff4d1fdb0a494e296be0a6708e5a22fc72f14587442fb7f8f7906
```

The module remains a separate file beside the application. The application pins its exact audited hash before asking the signed official PawnIO driver to load it. Full corresponding module source and license are included.

## Reference implementations

The read protocol and AXB35 register map were independently checked against:

- KyaniteLabs/evo-x2-ec — persistent register map
- deseven/ec-su_axb35-win — Windows AXB35 EC behavior
- pajtony/FanControl.AXB35 — PawnIO ACPI EC access pattern, MIT
- LibreHardwareMonitor 0.9.6 — PawnIO client protocol, MPL-2.0

No LibreHardwareMonitor binary or source file is shipped in the runtime package.
