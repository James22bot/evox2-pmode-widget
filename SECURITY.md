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
- The GUI is read-only at the EC-register level, but ACPI EC port access still has residual concurrency risk with Windows ACPI.sys.

## Hardening invariants

- No ACPI EC command `0x81`.
- No arbitrary command, path, or payload accepted through the tray callback.
- P-MODE polling never leaves a stale valid value visible after a read failure.
- Program Files is the only supported installed execution location.
- Scheduled-task action path, working directory, principal, trigger, and run level must be read back after registration.
