# Changelog

## 0.3.3-beta.4

- Keep the widget running when Explorer is not yet ready at logon and recover tray registration through the existing refresh loop.
- Recover the notification-area icon after Explorer restarts without issuing duplicate icon additions.

## 0.3.3-beta.3

- Add explicit tray-menu switching between Quiet, Balanced, and Performance.
- Restrict writes to the hardware-verified EC firmware 1.08 and fixed register `0x31` values.
- Require a current-mode confirmation, at most one write attempt, and two stable target readbacks.
- Fail closed without automatic write retry or rollback when the outcome is uncertain, and quarantine all further writes for that program run.
- Reject reentrant mode-change requests while confirmation is open and recheck quarantine immediately before the EC transaction.
- Harden Windows installer CI with run-unique evidence logs, dirty-preflight preservation, process-start tracing, and scheduled-task run-time proof.

## 0.3.3-beta.2

- Fix the optional post-install launch after a normal double-click by explicitly retaining Setup's approved elevated token.
- Preserve UAC and the existing per-user highest-privilege logon-task security model.

## 0.3.3-beta.1

- Initial public beta.
- Compact 132 × 24 desktop-layer widget.
- Persistent tray context menu owned by a dedicated control window.
- Read-only P-MODE polling through official PawnIO.
- Inno Setup installer with Start-menu shortcut and per-user highest-privilege logon task.
- Portable ZIP, source archive, and SHA-256 release manifest.
