# Mono runtime for the Linux FOMOD helper

Framework Mono 6.14.1 with its compiler and private native libraries, built for glibc 2.35+ x64.
The FOMOD helper itself is built from the `linux` branch and ships in `@nexusmods/fomod-installer-ipc` (`dist/mono`).

- `mono-runtime-linux-x64.zip`: content-addressed objects (`objects/<sha256>`)
- `mono-runtime-linux-x64.json`: path, mode, size and SHA-256 of every file, with provenance

Vortex downloads both by commit-pinned URL and verifies their SHA-256.
