# Security Policy

## Supported versions

audiaki is a small tool with a single active line of development. Fixes land on
`main` and go out in the next tagged release.

| Version | Supported |
| --- | --- |
| 0.2.x | Yes |
| < 0.2 | No |

## Reporting a vulnerability

Please report privately rather than in a public issue:

- Open a [private security advisory](https://github.com/HilthonTT/audiaki/security/advisories/new), or
- Contact the repository owner through their GitHub profile.

Include the version (`audiaki --version`), the invocation, and a WAV file or
device configuration that triggers the problem if one exists.

You can expect an acknowledgement within a week. Fixes for confirmed issues are
released as soon as they are ready, and you will be credited in the changelog
unless you prefer otherwise.

## Threat model

audiaki runs unprivileged, reads from an ALSA capture device, and writes a file
whose path the user supplies. It parses no untrusted file input and opens no
network sockets, so the realistic attack surface is small:

- Command line and `AUDIAKI_DEVICE` handling
- The device string passed to libasound
- Buffer arithmetic in the capture, repack and WAV write path

Memory-safety bugs in that path — buffer overflows, integer overflows on the
sample-count arithmetic, writes past the output buffer — are the reports of
most interest. CI runs the test suite under AddressSanitizer and UBSan.

Note that `make install` writes to `/usr/local` and needs privileges; that is
ordinary installation behaviour, not a vulnerability.
