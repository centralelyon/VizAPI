// Copyright 2026, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Authors: Patrick Brosi <brosi@informatik.uni-freiburg.de>

#ifndef UTIL_PROTOBUF_PROTOBUF_H_
#define UTIL_PROTOBUF_PROTOBUF_H_

#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

namespace util {
namespace protobuf {

enum VarType : uint8_t {
  V = 0,
  D = 1,
  S = 2,
  I = 5,
};

// _____________________________________________________________________________
inline uint64_t parseVarUInt(unsigned char*& c) {
  int i = 0;
  uint64_t ret = 0;
  uint64_t cur;

  do {
    cur = *c;
    ret |= (uint64_t)((uint8_t)(cur << 1) >> 1) << (i * 7);
    i++;
    c++;
  } while (cur & (1 << 7));

  return ret;
}

// _____________________________________________________________________________
inline int64_t parseVarInt(unsigned char*& c) {
  int64_t i = parseVarUInt(c);
  return (i >> 1) ^ (-(i & 1));
}

// _____________________________________________________________________________
inline std::pair<VarType, uint8_t> nextTypeAndId(unsigned char*& c) {
  uint64_t varInt = parseVarUInt(c);
  auto type = VarType(((unsigned char)(varInt << 5) >> 5));
  return {type, (varInt << 1) >> 4};
}

// _____________________________________________________________________________
inline uint32_t parseFixedUInt32(unsigned char*& c) {
  // manual assembly to ensure correct endianess
  uint32_t ret =
      (static_cast<uint32_t>(static_cast<unsigned char>(c[0]))) |
      (static_cast<uint32_t>(static_cast<unsigned char>(c[1])) << 8) |
      (static_cast<uint32_t>(static_cast<unsigned char>(c[2])) << 16) |
      (static_cast<uint32_t>(static_cast<unsigned char>(c[3])) << 24);
  c += 4;
  return ret;
}

// _____________________________________________________________________________
inline uint64_t parseFixedUInt64(unsigned char*& c) {
  // manual assembly to ensure correct endianess
  uint64_t ret =
      (static_cast<uint64_t>(static_cast<unsigned char>(c[0]))) |
      (static_cast<uint64_t>(static_cast<unsigned char>(c[1])) << 8) |
      (static_cast<uint64_t>(static_cast<unsigned char>(c[2])) << 16) |
      (static_cast<uint64_t>(static_cast<unsigned char>(c[3])) << 24) |
      (static_cast<uint64_t>(static_cast<unsigned char>(c[4])) << 32) |
      (static_cast<uint64_t>(static_cast<unsigned char>(c[5])) << 40) |
      (static_cast<uint64_t>(static_cast<unsigned char>(c[6])) << 48) |
      (static_cast<uint64_t>(static_cast<unsigned char>(c[7])) << 56);
  c += 8;
  return ret;
}

// _____________________________________________________________________________
inline int32_t parseFixedInt32(unsigned char*& c) {
  uint32_t i = parseFixedUInt32(c);
  return (i >> 1) ^ (-(i & 1));
}

// _____________________________________________________________________________
inline int64_t parseFixedInt64(unsigned char*& c) {
  uint32_t i = parseFixedUInt64(c);
  return (i >> 1) ^ (-(i & 1));
}

// _____________________________________________________________________________
inline uint64_t parseUInt(std::pair<VarType, uint8_t> typeId,
                          unsigned char*& c) {
  if (typeId.first == VarType::I) return parseFixedUInt32(c);
  if (typeId.first == VarType::D) return parseFixedUInt64(c);
  if (typeId.first == VarType::V) return parseVarUInt(c);
  throw std::runtime_error("expected unsigned integer value");
}

// _____________________________________________________________________________
inline int64_t parseInt(std::pair<VarType, uint8_t> typeId, unsigned char*& c) {
  if (typeId.first == VarType::I) return parseFixedInt32(c);
  if (typeId.first == VarType::D) return parseFixedInt64(c);
  if (typeId.first == VarType::V) return parseVarInt(c);
  throw std::runtime_error("expected signed integer value");
}

// _____________________________________________________________________________
inline std::string parseString(unsigned char*& c) {
  auto len = parseVarUInt(c);
  std::string ret = {reinterpret_cast<const char*>(c), len};
  c += len;
  return ret;
}

// _____________________________________________________________________________
inline size_t varUIntNumBytes(uint64_t val) {
  if (val == 0) return 1;
  // 7 bit payload per byte
  return ceil((floor(log2(val)) + 1) / 7);
}

// _____________________________________________________________________________
inline size_t varIntNumBytes(int64_t val) {
  // 7 bit payload per byte
  return varUIntNumBytes((static_cast<uint64_t>(val) << 1) ^
                         static_cast<uint64_t>(val >> 63));
}

// _____________________________________________________________________________
inline void writeVarUInt(uint64_t val, unsigned char*& c) {
  while (val >= 128) {  // as long as we have a value that doesnt fit in 7 bits
    // take one byte, ignore highest bit and set to 1 (continue)
    *c = static_cast<uint8_t>(val) | 128;
    c++;
    // shift by the written 7 bits
    val = val >> 7;
  }

  // remaining last byte, with unset continuation bit
  *c = static_cast<uint8_t>(val);
  c++;
}

// _____________________________________________________________________________
inline void writeVarUInt(uint64_t val, std::ostream& out) {
  while (val >= 128) {  // as long as we have a value that doesnt fit in 7 bits
    // take one byte, ignore highest bit and set to 1 (continue)
    out.put(static_cast<uint8_t>(val) | 128);
    // shift by the written 7 bits
    val = val >> 7;
  }

  // remaining last byte, with unset continuation bit
  out.put(static_cast<uint8_t>(val));
}

// _____________________________________________________________________________
inline void writeVarInt(int64_t val, unsigned char*& c) {
  writeVarUInt(
      (static_cast<uint64_t>(val) << 1) ^ static_cast<uint64_t>(val >> 63), c);
}

// _____________________________________________________________________________
inline void writeVarInt(int64_t val, std::ostream& out) {
  writeVarUInt(
      (static_cast<uint64_t>(val) << 1) ^ static_cast<uint64_t>(val >> 63), out);
}

// _____________________________________________________________________________
inline void writeString(const std::string& str, unsigned char*& c) {
  writeVarUInt(str.size(), c);
  memcpy(c, str.c_str(), str.size());
  c += str.size();
}

// _____________________________________________________________________________
inline void writeString(const std::string& str, std::ostream& out) {
  writeVarUInt(str.size(), out);
  out.write(str.data(), str.size());
}

// _____________________________________________________________________________
inline void writeBuf(const unsigned char* src, size_t s, unsigned char*& c) {
  memcpy(c, src, s);
  c += s;
}

// _____________________________________________________________________________
inline void writeTypeAndId(const std::pair<VarType, uint8_t>& typeId,
                           unsigned char*& c) {
  uint64_t byte = 0;
  // set type
  byte |= typeId.first;
  // set id
  byte |= (static_cast<uint64_t>(typeId.second) << 3);

  writeVarUInt(byte, c);
}

// _____________________________________________________________________________
inline void writeTypeAndId(const std::pair<VarType, uint8_t>& typeId, std::ostream& out) {
  uint64_t byte = 0;
  // set type
  byte |= typeId.first;
  // set id
  byte |= (static_cast<uint64_t>(typeId.second) << 3);

  writeVarUInt(byte, out);
}

}  // namespace protobuf
}  // namespace util

#endif
