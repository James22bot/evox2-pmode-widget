# Contributing

Bug reports and focused pull requests are welcome.

## Development rules

1. Preserve the read-only EC contract. Do not add register writes or command `0x81`.
2. Keep the board identity gate and fail-closed telemetry validation.
3. Do not weaken PawnIO module hash verification or mutex handling.
4. Add behavior tests before implementation changes.
5. Run both host test suites and the Windows build.
6. Do not commit credentials, private host paths, build outputs, or user-specific runtime data.

## Verification

```bash
make test
make windows ZIG=/absolute/path/to/zig
```

Changes to installer/autostart behavior require explicit review of Program Files targets, task identity, rollback behavior, and uninstall cleanup.
