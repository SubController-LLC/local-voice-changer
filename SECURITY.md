# Security Policy

## Supported versions

The current v0.3.x public beta receives security fixes on the default branch. There is no stable
release yet; physical-device and long-running tests remain outstanding.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting form in this repository's **Security** tab. Do
not include exploit details, device names, or personal audio in a public issue. If private reporting
is unavailable, open a public issue containing only a request for a private contact channel.

## Distribution notes

- The application processes microphone samples locally and has no network or telemetry code.
- Public beta binaries are published only after the exact ZIP passes Windows CI, then receive a
  GitHub build-provenance attestation.
- Beta executables are not Authenticode-signed, so Windows SmartScreen can warn on first
  launch.
- VB-CABLE is not bundled. Download drivers only from VB-Audio's official website.
