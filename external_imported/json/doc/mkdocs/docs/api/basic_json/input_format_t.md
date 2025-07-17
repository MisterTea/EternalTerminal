# basic_json::input_format_t

```cpp
enum class input_format_t {
    json,
    cbor,
    msgpack,
    ubjson,
    bson
};
```

This enumeration is used in the [`sax_parse`](sax_parse.md) function to choose the input format to parse:

json
:   JSON (JavaScript Object Notation)

cbor
:   CBOR (Concise Binary Object Representation)

msgpack
:   MessagePack

ubjson
:   UBJSON (Universal Binary JSON)

bson
:   BSON (Bin­ary JSON)

## Version history

- Added in version 3.2.0.
