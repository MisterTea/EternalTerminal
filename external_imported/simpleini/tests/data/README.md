# UTF-8 conversion test data

## Differential tests (primary)

`tests/ts-utf8-conversion.cpp` compares `SI_UTF8::Encode` /
`Decode` against a platform reference implementation for every assigned Unicode
scalar value U+0000..U+10FFFF (excluding surrogate code points U+D800..U+DFFF).

On Linux and Windows the reference is `c32rtomb` / `mbrtoc32` from `<uchar.h>`
(with a UTF-8 locale). On macOS the reference is `iconv` (`UTF-32LE` ↔ `UTF-8`),
because Apple Clang does not expose the C23 `<uchar.h>` conversion functions in
C++ translation units.

For each code point the test checks:

1. Our UTF-8 bytes match the system library
2. System UTF-8 decodes back to the same code point
3. Our UTF-8 decodes back to the same code point

## External fixtures

| File | Purpose |
|------|---------|
| `utf8-reject.hex` | Invalid UTF-8 byte sequences (hex) that must fail `SI_UTF8::Decode` and `ConvertFromStore` / `LoadData`. Sourced from [Markus Kuhn UTF-8-test.txt](https://www.cl.cam.ac.uk/~mgk25/ucs/examples/UTF-8-test.txt) sections 3–5 (CC BY 4.0). |
| `utf8-ini-roundtrip.ini` | UTF-8 INI integration test via `CSimpleIniW` |

**utf8-reject.hex** — one byte sequence per line as lowercase hex. Lines
starting with `#` are ignored.
