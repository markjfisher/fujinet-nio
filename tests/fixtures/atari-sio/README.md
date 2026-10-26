# Atari SIO/NIO wire fixtures

Byte-exact golden vectors for the Atari SIO/NIO frontends (`nio_v1.json`,
`disk_v1.json`, their manifests, and the boot acceptance trace). This
repository defines the protocol, so it owns these files.

- The unit tests (`tests/test_atari_*.cpp`) read them through
  `FN_ATARI_SIO_FIXTURE_DIR`, set in `tests/CMakeLists.txt`. They must not
  reach outside the repository.
- Other repositories (the workspace acceptance tests, emulator tests) consume
  them from this location; they do not keep their own copies.
