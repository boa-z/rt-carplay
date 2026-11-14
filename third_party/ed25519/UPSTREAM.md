# Ed25519 (orlp/ed25519)

The C sources in this directory are the unmodified portable Ed25519
implementation from [`orlp/ed25519`](https://github.com/orlp/ed25519).
Upstream project revision: see the `esp32-carplay-web-relay` vendor copy this
snapshot was taken from (2026-11-08).

License: zlib (see `LICENSE.txt`).  The zlib license is compatible with this
repository's GPL-3.0-or-later licensing; the copyright and license notice is
retained verbatim.

Only `core/crypto/airplay_ed25519.c` calls into this library.  Seed generation
is deliberately not used here: `ed25519_create_seed()` reads the platform
entropy source, so the caller supplies the seed from `rtcp_os.random` (or a
hardware-backed identity) instead.
