# SHA-256

- **Version:** 1.0.0
- **Source:** Original implementation for the TokToken project
- **License:** Same as TokToken project (AGPL-3.0)
- **Created:** 2026-03-11

## Files

- `sha256.c` -- SHA-256 implementation based on FIPS PUB 180-4
- `sha256.h` -- Public API header

## API

```c
void tt_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void tt_sha256_hex(const uint8_t *data, size_t len, char out[65]);
```

## Modifications

N/A -- original implementation, not derived from external code.
