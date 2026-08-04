# Summary

<!-- What does this change and why? Link any issue with "Fixes #123". -->

## Type of change

- [ ] Bug fix
- [ ] New feature
- [ ] Refactor or cleanup
- [ ] Documentation
- [ ] Build / CI

## How was it tested?

<!--
Say what you actually ran. For anything touching capture, include the device
and the command, e.g.:

  make STRICT=1 check
  ./build/audiaki -D hw:CARD=Box,DEV=0 -t 10 take.wav   # 10.00 s, 0 xruns
-->

- [ ] `make STRICT=1 check` passes
- [ ] Tested against real hardware (device: ______)

## Checklist

- [ ] Code is formatted (`make format`)
- [ ] New behaviour is covered by tests in `tests/` where it does not need a device
- [ ] `--help` and `docs/audiaki.1` are updated if flags changed
- [ ] `CHANGELOG.md` has an entry under "Unreleased"
