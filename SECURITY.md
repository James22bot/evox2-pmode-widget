# Security Policy

## Supported versions

Only the latest published release is supported.

## Reporting a vulnerability

Use GitHub's private security advisory feature for vulnerabilities. Do not include credentials, tokens, private documents, or unrelated host data in a report.

## Trust boundaries

- The EC register map is reverse-engineered and hardware-specific.
- PawnIO is a universal kernel hardware-access driver with broader capability than this widget.
- Use only the official signed PawnIO release and keep Secure Boot enabled.
- Release binaries are currently not Authenticode-signed. Verify `SHA256SUMS.txt` before running setup or portable binaries.
- Tagged release artifacts carry GitHub build-provenance attestations. Verify them with `gh attestation verify <artifact> --repo James22bot/evox2-pmode-widget`.
- The installer creates a highest-privilege current-user logon task because the official PawnIO ACL normally rejects non-elevated processes.
- Install from the intended local administrator account. Over-the-shoulder elevation into a different administrator identity is unsupported in this beta.
- Telemetry is read-only. A user-confirmed mode change permits one tightly constrained write to EC register `0x31` on the hardware-verified EC firmware 1.08.
- ACPI EC port access, including readback-verified writes, still has residual concurrency and firmware risk with Windows ACPI.sys.

## Hardening invariants

- ACPI EC command `0x81` is allowed only for an explicit user-confirmed mode change, at most once per request.
- The only writable register is `0x31`; the only writable values are `0x00`, `0x01`, and `0x02` through the typed `PMode` contract.
- Writes require EC firmware 1.08, stable current-mode revalidation under `Global\\Access_EC`, a 200 ms settle, and two matching target readbacks.
- A write or readback failure is outcome-indeterminate: the first anomaly trips a non-resettable process-lifetime write quarantine, with no automatic retry or rollback write.
- A process-wide mode-change gate rejects nested tray callbacks while confirmation is open; quarantine is checked again after confirmation and immediately before entering the EC backend.
- Status polling may recover the displayed mode after an indeterminate outcome, but it cannot clear the write quarantine. Further writes require manually establishing a stable hardware state and restarting the widget.
- No arbitrary command, path, or payload accepted through the tray callback.
- P-MODE polling never leaves a stale valid value visible after a read failure.
- Program Files is the only supported installed execution location.
- Scheduled-task action path, working directory, principal, trigger, and run level must be read back after registration.
