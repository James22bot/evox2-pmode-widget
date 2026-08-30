# Contributing

Bug reports and focused pull requests are welcome.

## Development rules

1. Preserve the closed P-MODE write contract: only register `0x31`, typed values `0x00`–`0x02`, firmware 1.08, at most one confirmed attempt, stable target readback, and process-lifetime write quarantine after an indeterminate outcome.
2. Keep the board identity gate and fail-closed telemetry validation.
3. Do not weaken PawnIO module hash verification, full-transaction mutex handling, or the no-retry/no-rollback rule.
4. Add behavior tests before implementation changes.
5. Run both host test suites and the Windows build.
6. Do not commit credentials, private host paths, build outputs, or user-specific runtime data.

## Verification

```bash
make test
make windows ZIG=/absolute/path/to/zig
```

Changes to installer/autostart behavior require explicit review of Program Files targets, task identity, rollback behavior, and uninstall cleanup.
