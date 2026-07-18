// Copyright 2020 the V8 project authors.
//
// This is the small portion of V8's fast-call declaration surface required by
// Caeneus. Node distributes the corresponding declarations inconsistently
// across releases, so the addon keeps the ABI declarations local while using
// the V8 implementation supplied by the Node runtime.

#ifndef CAENEUS_V8_FAST_API_CALLS_H_
#define CAENEUS_V8_FAST_API_CALLS_H_

#include <cstddef>
#include <cstdint>

#include "v8-local-handle.h"
#include "v8config.h"

namespace v8 {

class CTypeInfo {
 public:
  enum class Type : uint8_t {
    kVoid,
    kBool,
    kUint8,
    kInt32,
    kUint32,
    kInt64,
    kUint64,
    kFloat32,
    kFloat64,
    kPointer,
    kV8Value,
    kSeqOneByteString,
    kApiObject,
    kAny,
  };

  enum class SequenceType : uint8_t {
    kScalar,
    kIsSequence,
    kIsTypedArray,
    kIsArrayBuffer,
  };

  enum class Flags : uint8_t {
    kNone = 0,
    kAllowSharedBit = 1 << 0,
    kEnforceRangeBit = 1 << 1,
    kClampBit = 1 << 2,
    kIsRestrictedBit = 1 << 3,
  };

  static constexpr Type kCallbackOptionsType = Type(255);

  explicit constexpr CTypeInfo(
      Type type,
      SequenceType sequence_type = SequenceType::kScalar,
      Flags flags = Flags::kNone)
      : type_(type), sequence_type_(sequence_type), flags_(flags) {}

  constexpr Type GetType() const { return type_; }
  constexpr SequenceType GetSequenceType() const { return sequence_type_; }
  constexpr Flags GetFlags() const { return flags_; }

 private:
  Type type_;
  SequenceType sequence_type_;
  Flags flags_;
};

struct FastOneByteString {
  const char* data;
  uint32_t length;
};

struct FastApiTypedArrayBase {
  size_t length_ = 0;

  size_t length() const { return length_; }
};

template <typename T>
struct FastApiTypedArray : public FastApiTypedArrayBase {
  bool getStorageIfAligned(T** elements) const {
    if (reinterpret_cast<uintptr_t>(data_) % alignof(T) != 0) {
      return false;
    }
    *elements = reinterpret_cast<T*>(data_);
    return true;
  }

 private:
  void* data_ = nullptr;
};

class V8_EXPORT CFunctionInfo {
 public:
  enum class Int64Representation : uint8_t {
    kNumber = 0,
    kBigInt = 1,
  };

  CFunctionInfo(
      const CTypeInfo& return_info,
      unsigned int arg_count,
      const CTypeInfo* arg_info,
      Int64Representation repr = Int64Representation::kNumber);

  const CTypeInfo& ReturnInfo() const { return return_info_; }
  const CTypeInfo& ArgumentInfo(unsigned int index) const;
  unsigned int ArgumentCount() const {
    return HasOptions() ? arg_count_ - 1 : arg_count_;
  }
  Int64Representation GetInt64Representation() const { return repr_; }

 private:
  bool HasOptions() const {
    return arg_count_ > 0 &&
           arg_info_[arg_count_ - 1].GetType() ==
               CTypeInfo::kCallbackOptionsType;
  }

  const CTypeInfo return_info_;
  const Int64Representation repr_;
  const unsigned int arg_count_;
  const CTypeInfo* arg_info_;
};

class V8_EXPORT CFunction {
 public:
  constexpr CFunction() : address_(nullptr), type_info_(nullptr) {}

  CFunction(const void* address, const CFunctionInfo* type_info)
      : address_(address), type_info_(type_info) {}

  const CTypeInfo& ReturnInfo() const { return type_info_->ReturnInfo(); }
  const CTypeInfo& ArgumentInfo(unsigned int index) const {
    return type_info_->ArgumentInfo(index);
  }
  unsigned int ArgumentCount() const { return type_info_->ArgumentCount(); }
  const void* GetAddress() const { return address_; }
  CFunctionInfo::Int64Representation GetInt64Representation() const {
    return type_info_->GetInt64Representation();
  }
  const CFunctionInfo* GetTypeInfo() const { return type_info_; }

 private:
  const void* address_;
  const CFunctionInfo* type_info_;
};

}  // namespace v8

#endif  // CAENEUS_V8_FAST_API_CALLS_H_
