// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_NUMERICS_BYTE_CONVERSIONS_H_
#define MINI_CHROMIUM_BASE_NUMERICS_BYTE_CONVERSIONS_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "base/numerics/basic_ops_impl.h"
#include "build/build_config.h"

// Chromium only builds and runs on Little Endian machines.
static_assert(ARCH_CPU_LITTLE_ENDIAN);

namespace base::numerics {

// Returns a value with all bytes in |x| swapped, i.e. reverses the endianness.
// TODO(pkasting): Once C++23 is available, replace with std::byteswap.
template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
inline constexpr T ByteSwap(T value) {
  return numerics::internal::SwapBytes(value);
}

// Returns a uint8_t with the value in `bytes` interpreted as the native endian
// encoding of the integer for the machine.
//
// This is suitable for decoding integers that were always kept in native
// encoding, such as when stored in shared-memory (or through IPC) as a byte
// buffer. Prefer an explicit little endian when storing and reading data from
// storage, and explicit big endian for network order.
//
// Note that since a single byte can have only one ordering, this just copies
// the byte out of the span. This provides a consistent function for the
// operation nonetheless.
inline constexpr uint8_t U8FromNativeEndian(
    base::span<const uint8_t, 1u> bytes) {
  return bytes[0];
}
// Returns a uint16_t with the value in `bytes` interpreted as the native endian
// encoding of the integer for the machine.
//
// This is suitable for decoding integers that were always kept in native
// encoding, such as when stored in shared-memory (or through IPC) as a byte
// buffer. Prefer an explicit little endian when storing and reading data from
// storage, and explicit big endian for network order.
inline constexpr uint16_t U16FromNativeEndian(
    base::span<const uint8_t, 2u> bytes) {
  return internal::FromLittleEndian<uint16_t>(bytes);
}
// Returns a uint32_t with the value in `bytes` interpreted as the native endian
// encoding of the integer for the machine.
//
// This is suitable for decoding integers that were always kept in native
// encoding, such as when stored in shared-memory (or through IPC) as a byte
// buffer. Prefer an explicit little endian when storing and reading data from
// storage, and explicit big endian for network order.
inline constexpr uint32_t U32FromNativeEndian(
    base::span<const uint8_t, 4u> bytes) {
  return internal::FromLittleEndian<uint32_t>(bytes);
}
// Returns a uint64_t with the value in `bytes` interpreted as the native endian
// encoding of the integer for the machine.
//
// This is suitable for decoding integers that were always kept in native
// encoding, such as when stored in shared-memory (or through IPC) as a byte
// buffer. Prefer an explicit little endian when storing and reading data from
// storage, and explicit big endian for network order.
inline constexpr uint64_t U64FromNativeEndian(
    base::span<const uint8_t, 8u> bytes) {
  return internal::FromLittleEndian<uint64_t>(bytes);
}

template <class To,
          class From,
          std::enable_if_t<sizeof(To) == sizeof(From) &&
                               std::is_trivially_copyable_v<From> &&
                               std::is_trivially_copyable_v<To> &&
                               std::is_trivially_constructible_v<To>,
                           int> = 0>
inline constexpr To BitCast(const From& src) noexcept {
  To dst;
  std::memcpy(&dst, &src, sizeof(To));
  return dst;
}

// Returns a float with the value in `bytes` interpreted as the native endian
// encoding of the number for the machine.
//
// This is suitable for decoding numbers that were always kept in native
// encoding, such as when stored in shared-memory (or through IPC) as a byte
// buffer. Prefer an explicit little endian when storing and reading data from
// storage, and explicit big endian for network order.
inline constexpr float FloatFromNativeEndian(
    base::span<const uint8_t, 4u> bytes) {
  return BitCast<float>(U32FromNativeEndian(bytes));
}
// Returns a double with the value in `bytes` interpreted as the native endian
// encoding of the number for the machine.
//
// This is suitable for decoding numbers that were always kept in native
// encoding, such as when stored in shared-memory (or through IPC) as a byte
// buffer. Prefer an explicit little endian when storing and reading data from
// storage, and explicit big endian for network order.
inline constexpr double DoubleFromNativeEndian(
    base::span<const uint8_t, 8u> bytes) {
  return BitCast<double>(U64FromNativeEndian(bytes));
}

// Returns a uint8_t with the value in `bytes` interpreted as a little-endian
// encoding of the integer.
//
// This is suitable for decoding integers encoded explicitly in little endian,
// which is a good practice with storing and reading data from storage. Use
// the native-endian versions when working with values that were always in
// memory, such as when stored in shared-memory (or through IPC) as a byte
// buffer.
//
// Note that since a single byte can have only one ordering, this just copies
// the byte out of the span. This provides a consistent function for the
// operation nonetheless.
inline constexpr uint8_t U8FromLittleEndian(
    base::span<const uint8_t, 1u> bytes) {
  return bytes[0];
}
// Returns a uint16_t with the value in `bytes` interpreted as a little-endian
// encoding of the integer.
//
// This is suitable for decoding integers encoded explicitly in little endian,
// which is a good practice with storing and reading data from storage. Use
// the native-endian versions when working with values that were always in
// memory, such as when stored in shared-memory (or through IPC) as a byte
// buffer.
inline constexpr uint16_t U16FromLittleEndian(
    base::span<const uint8_t, 2u> bytes) {
  return internal::FromLittleEndian<uint16_t>(bytes);
}
// Returns a uint32_t with the value in `bytes` interpreted as a little-endian
// encoding of the integer.
//
// This is suitable for decoding integers encoded explicitly in little endian,
// which is a good practice with storing and reading data from storage. Use
// the native-endian versions when working with values that were always in
// memory, such as when stored in shared-memory (or through IPC) as a byte
// buffer.
inline constexpr uint32_t U32FromLittleEndian(
    base::span<const uint8_t, 4u> bytes) {
  return internal::FromLittleEndian<uint32_t>(bytes);
}
// Returns a uint64_t with the value in `bytes` interpreted as a little-endian
// encoding of the integer.
//
// This is suitable for decoding integers encoded explicitly in little endian,
// which is a good practice with storing and reading data from storage. Use
// the native-endian versions when working with values that were always in
// memory, such as when stored in shared-memory (or through IPC) as a byte
// buffer.
inline constexpr uint64_t U64FromLittleEndian(
    base::span<const uint8_t, 8u> bytes) {
  return internal::FromLittleEndian<uint64_t>(bytes);
}
// Returns a float with the value in `bytes` interpreted as a little-endian
// encoding of the integer.
//
// This is suitable for decoding numbers encoded explicitly in little endian,
// which is a good practice with storing and reading data from storage. Use
// the native-endian versions when working with values that were always in
// memory, such as when stored in shared-memory (or through IPC) as a byte
// buffer.
inline constexpr float FloatFromLittleEndian(
    base::span<const uint8_t, 4u> bytes) {
  return BitCast<float>(U32FromLittleEndian(bytes));
}
// Returns a double with the value in `bytes` interpreted as a little-endian
// encoding of the integer.
//
// This is suitable for decoding numbers encoded explicitly in little endian,
// which is a good practice with storing and reading data from storage. Use
// the native-endian versions when working with values that were always in
// memory, such as when stored in shared-memory (or through IPC) as a byte
// buffer.
inline constexpr double DoubleFromLittleEndian(
    base::span<const uint8_t, 8u> bytes) {
  return BitCast<double>(U64FromLittleEndian(bytes));
}

// Returns a uint8_t with the value in `bytes` interpreted as a big-endian
// encoding of the integer.
//
// This is suitable for decoding integers encoded explicitly in big endian, such
// as for network order. Use the native-endian versions when working with values
// that were always in memory, such as when stored in shared-memory (or through
// IPC) as a byte buffer.
//
// Note that since a single byte can have only one ordering, this just copies
// the byte out of the span. This provides a consistent function for the
// operation nonetheless.
inline constexpr uint8_t U8FromBigEndian(base::span<const uint8_t, 1u> bytes) {
  return bytes[0];
}
// Returns a uint16_t with the value in `bytes` interpreted as a big-endian
// encoding of the integer.
//
// This is suitable for decoding integers encoded explicitly in big endian, such
// as for network order. Use the native-endian versions when working with values
// that were always in memory, such as when stored in shared-memory (or through
// IPC) as a byte buffer.
inline constexpr uint16_t U16FromBigEndian(
    base::span<const uint8_t, 2u> bytes) {
  return ByteSwap(internal::FromLittleEndian<uint16_t>(bytes));
}
// Returns a uint32_t with the value in `bytes` interpreted as a big-endian
// encoding of the integer.
//
// This is suitable for decoding integers encoded explicitly in big endian, such
// as for network order. Use the native-endian versions when working with values
// that were always in memory, such as when stored in shared-memory (or through
// IPC) as a byte buffer.
inline constexpr uint32_t U32FromBigEndian(
    base::span<const uint8_t, 4u> bytes) {
  return ByteSwap(internal::FromLittleEndian<uint32_t>(bytes));
}
// Returns a uint64_t with the value in `bytes` interpreted as a big-endian
// encoding of the integer.
//
// This is suitable for decoding integers encoded explicitly in big endian, such
// as for network order. Use the native-endian versions when working with values
// that were always in memory, such as when stored in shared-memory (or through
// IPC) as a byte buffer.
inline constexpr uint64_t U64FromBigEndian(
    base::span<const uint8_t, 8u> bytes) {
  return ByteSwap(internal::FromLittleEndian<uint64_t>(bytes));
}
// Returns a float with the value in `bytes` interpreted as a big-endian
// encoding of the integer.
//
// This is suitable for decoding numbers encoded explicitly in big endian, such
// as for network order. Use the native-endian versions when working with values
// that were always in memory, such as when stored in shared-memory (or through
// IPC) as a byte buffer.
inline constexpr float FloatFromBigEndian(base::span<const uint8_t, 4u> bytes) {
  return BitCast<float>(U32FromBigEndian(bytes));
}
// Returns a double with the value in `bytes` interpreted as a big-endian
// encoding of the integer.
//
// This is suitable for decoding numbers encoded explicitly in big endian, such
// as for network order. Use the native-endian versions when working with values
// that were always in memory, such as when stored in shared-memory (or through
// IPC) as a byte buffer.
inline constexpr double DoubleFromBigEndian(
    base::span<const uint8_t, 8u> bytes) {
  return BitCast<double>(U64FromBigEndian(bytes));
}

// Returns a byte array holding the value of a uint8_t encoded as the native
// endian encoding of the integer for the machine.
//
// This is suitable for encoding integers that will always be kept in native
// encoding, such as for storing in shared-memory (or sending through IPC) as a
// byte buffer. Prefer an explicit little endian when storing data into external
// storage, and explicit big endian for network order.
inline constexpr std::array<uint8_t, 1u> U8ToNativeEndian(uint8_t val) {
  return {val};
}
// Returns a byte array holding the value of a uint16_t encoded as the native
// endian encoding of the integer for the machine.
//
// This is suitable for encoding integers that will always be kept in native
// encoding, such as for storing in shared-memory (or sending through IPC) as a
// byte buffer. Prefer an explicit little endian when storing data into external
// storage, and explicit big endian for network order.
inline constexpr std::array<uint8_t, 2u> U16ToNativeEndian(uint16_t val) {
  return internal::ToLittleEndian(val);
}
// Returns a byte array holding the value of a uint32_t encoded as the native
// endian encoding of the integer for the machine.
//
// This is suitable for encoding integers that will always be kept in native
// encoding, such as for storing in shared-memory (or sending through IPC) as a
// byte buffer. Prefer an explicit little endian when storing data into external
// storage, and explicit big endian for network order.
inline constexpr std::array<uint8_t, 4u> U32ToNativeEndian(uint32_t val) {
  return internal::ToLittleEndian(val);
}
// Returns a byte array holding the value of a uint64_t encoded as the native
// endian encoding of the integer for the machine.
//
// This is suitable for encoding integers that will always be kept in native
// encoding, such as for storing in shared-memory (or sending through IPC) as a
// byte buffer. Prefer an explicit little endian when storing data into external
// storage, and explicit big endian for network order.
inline constexpr std::array<uint8_t, 8u> U64ToNativeEndian(uint64_t val) {
  return internal::ToLittleEndian(val);
}
// Returns a byte array holding the value of a float encoded as the native
// endian encoding of the number for the machine.
//
// This is suitable for encoding numbers that will always be kept in native
// encoding, such as for storing in shared-memory (or sending through IPC) as a
// byte buffer. Prefer an explicit little endian when storing data into external
// storage, and explicit big endian for network order.
inline constexpr std::array<uint8_t, 4u> FloatToNativeEndian(float val) {
  return U32ToNativeEndian(BitCast<uint32_t>(val));
}
// Returns a byte array holding the value of a double encoded as the native
// endian encoding of the number for the machine.
//
// This is suitable for encoding numbers that will always be kept in native
// encoding, such as for storing in shared-memory (or sending through IPC) as a
// byte buffer. Prefer an explicit little endian when storing data into external
// storage, and explicit big endian for network order.
inline constexpr std::array<uint8_t, 8u> DoubleToNativeEndian(double val) {
  return U64ToNativeEndian(BitCast<uint64_t>(val));
}

// Returns a byte array holding the value of a uint8_t encoded as the
// little-endian encoding of the integer.
//
// This is suitable for encoding integers explicitly in little endian, which is
// a good practice with storing and reading data from storage. Use the
// native-endian versions when working with values that will always be in
// memory, such as when stored in shared-memory (or passed through IPC) as a
// byte buffer.
inline constexpr std::array<uint8_t, 1u> U8ToLittleEndian(uint8_t val) {
  return {val};
}
// Returns a byte array holding the value of a uint16_t encoded as the
// little-endian encoding of the integer.
//
// This is suitable for encoding integers explicitly in little endian, which is
// a good practice with storing and reading data from storage. Use the
// native-endian versions when working with values that will always be in
// memory, such as when stored in shared-memory (or passed through IPC) as a
// byte buffer.
inline constexpr std::array<uint8_t, 2u> U16ToLittleEndian(uint16_t val) {
  return internal::ToLittleEndian(val);
}
// Returns a byte array holding the value of a uint32_t encoded as the
// little-endian encoding of the integer.
//
// This is suitable for encoding integers explicitly in little endian, which is
// a good practice with storing and reading data from storage. Use the
// native-endian versions when working with values that will always be in
// memory, such as when stored in shared-memory (or passed through IPC) as a
// byte buffer.
inline constexpr std::array<uint8_t, 4u> U32ToLittleEndian(uint32_t val) {
  return internal::ToLittleEndian(val);
}
// Returns a byte array holding the value of a uint64_t encoded as the
// little-endian encoding of the integer.
//
// This is suitable for encoding integers explicitly in little endian, which is
// a good practice with storing and reading data from storage. Use the
// native-endian versions when working with values that will always be in
// memory, such as when stored in shared-memory (or passed through IPC) as a
// byte buffer.
inline constexpr std::array<uint8_t, 8u> U64ToLittleEndian(uint64_t val) {
  return internal::ToLittleEndian(val);
}
// Returns a byte array holding the value of a float encoded as the
// little-endian encoding of the number.
//
// This is suitable for encoding numbers explicitly in little endian, which is
// a good practice with storing and reading data from storage. Use the
// native-endian versions when working with values that will always be in
// memory, such as when stored in shared-memory (or passed through IPC) as a
// byte buffer.
inline constexpr std::array<uint8_t, 4u> FloatToLittleEndian(float val) {
  return internal::ToLittleEndian(BitCast<uint32_t>(val));
}
// Returns a byte array holding the value of a double encoded as the
// little-endian encoding of the number.
//
// This is suitable for encoding numbers explicitly in little endian, which is
// a good practice with storing and reading data from storage. Use the
// native-endian versions when working with values that will always be in
// memory, such as when stored in shared-memory (or passed through IPC) as a
// byte buffer.
inline constexpr std::array<uint8_t, 8u> DoubleToLittleEndian(double val) {
  return internal::ToLittleEndian(BitCast<uint64_t>(val));
}

// Returns a byte array holding the value of a uint8_t encoded as the big-endian
// encoding of the integer.
//
// This is suitable for encoding integers explicitly in big endian, such as for
// network order. Use the native-endian versions when working with values that
// are always in memory, such as when stored in shared-memory (or passed through
// IPC) as a byte buffer. Use the little-endian encoding for storing and reading
// from storage.
inline constexpr std::array<uint8_t, 1u> U8ToBigEndian(uint8_t val) {
  return {val};
}
// Returns a byte array holding the value of a uint16_t encoded as the
// big-endian encoding of the integer.
//
// This is suitable for encoding integers explicitly in big endian, such as for
// network order. Use the native-endian versions when working with values that
// are always in memory, such as when stored in shared-memory (or passed through
// IPC) as a byte buffer. Use the little-endian encoding for storing and reading
// from storage.
inline constexpr std::array<uint8_t, 2u> U16ToBigEndian(uint16_t val) {
  return internal::ToLittleEndian(ByteSwap(val));
}
// Returns a byte array holding the value of a uint32_t encoded as the
// big-endian encoding of the integer.
//
// This is suitable for encoding integers explicitly in big endian, such as for
// network order. Use the native-endian versions when working with values that
// are always in memory, such as when stored in shared-memory (or passed through
// IPC) as a byte buffer. Use the little-endian encoding for storing and reading
// from storage.
inline constexpr std::array<uint8_t, 4u> U32ToBigEndian(uint32_t val) {
  return internal::ToLittleEndian(ByteSwap(val));
}
// Returns a byte array holding the value of a uint64_t encoded as the
// big-endian encoding of the integer.
//
// This is suitable for encoding integers explicitly in big endian, such as for
// network order. Use the native-endian versions when working with values that
// are always in memory, such as when stored in shared-memory (or passed through
// IPC) as a byte buffer. Use the little-endian encoding for storing and reading
// from storage.
inline constexpr std::array<uint8_t, 8u> U64ToBigEndian(uint64_t val) {
  return internal::ToLittleEndian(ByteSwap(val));
}
// Returns a byte array holding the value of a float encoded as the big-endian
// encoding of the number.
//
// This is suitable for encoding numbers explicitly in big endian, such as for
// network order. Use the native-endian versions when working with values that
// are always in memory, such as when stored in shared-memory (or passed through
// IPC) as a byte buffer. Use the little-endian encoding for storing and reading
// from storage.
inline constexpr std::array<uint8_t, 4u> FloatToBigEndian(float val) {
  return internal::ToLittleEndian(ByteSwap(BitCast<uint32_t>(val)));
}
// Returns a byte array holding the value of a double encoded as the big-endian
// encoding of the number.
//
// This is suitable for encoding numbers explicitly in big endian, such as for
// network order. Use the native-endian versions when working with values that
// are always in memory, such as when stored in shared-memory (or passed through
// IPC) as a byte buffer. Use the little-endian encoding for storing and reading
// from storage.
inline constexpr std::array<uint8_t, 8u> DoubleToBigEndian(double val) {
  return internal::ToLittleEndian(ByteSwap(BitCast<uint64_t>(val)));
}

}  // namespace base::numerics

#endif  //  MINI_CHROMIUM_BASE_NUMERICS_BYTE_CONVERSIONS_H_
