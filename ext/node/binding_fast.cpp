#include <node.h>
#include <node_buffer.h>
#include <v8.h>

#include "v8-fast-api-calls.h"

#include <caeneus.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr size_t kScratchSize = 4 * 1024 * 1024;
constexpr int32_t kFastMiss = -1;
constexpr int32_t kFastFallback = -2;

struct ByteView {
  const unsigned char* data = nullptr;
  size_t length = 0;
};

struct FastCache {
  void* handle = nullptr;
  std::vector<unsigned char> scratch;
  v8::Global<v8::Object> wrapper;
  v8::Global<v8::Object> scratch_buffer;
  std::string key_storage;
  std::string value_storage;
  bool closed = false;

  void close() {
    if (closed) {
      return;
    }
    closed = true;
    if (handle != nullptr) {
      caeneus_deinit(handle);
      handle = nullptr;
    }
  }
};

FastCache* get_cache(v8::Local<v8::Object> receiver) {
  if (receiver.IsEmpty() || receiver->InternalFieldCount() < 2) {
    return nullptr;
  }
  return static_cast<FastCache*>(
      receiver->GetAlignedPointerFromInternalField(1));
}

bool require_cache(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    FastCache** result) {
  *result = get_cache(args.This());
  if (*result == nullptr || (*result)->closed || (*result)->handle == nullptr) {
    args.GetIsolate()->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(
            args.GetIsolate(), "caeneus cache is closed")));
    return false;
  }
  return true;
}

bool read_bytes(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    std::string* storage,
    ByteView* result) {
  if (node::Buffer::HasInstance(value)) {
    result->data = reinterpret_cast<const unsigned char*>(
        node::Buffer::Data(value));
    result->length = node::Buffer::Length(value);
    return true;
  }

  if (value->IsUint8Array()) {
    auto typed_array = value.As<v8::Uint8Array>();
    std::shared_ptr<v8::BackingStore> backing =
        typed_array->Buffer()->GetBackingStore();
    result->data = static_cast<const unsigned char*>(backing->Data()) +
                   typed_array->ByteOffset();
    result->length = typed_array->ByteLength();
    return true;
  }

  if (!value->IsString()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(
            isolate, "key and value must be a string or Buffer")));
    return false;
  }

  v8::String::Utf8Value utf8(isolate, value);
  if (*utf8 == nullptr) {
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "string conversion failed")));
    return false;
  }
  storage->assign(*utf8, static_cast<size_t>(utf8.length()));
  result->data = reinterpret_cast<const unsigned char*>(storage->data());
  result->length = storage->size();
  return true;
}

bool read_buffer_output(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    unsigned char** data,
    size_t* length) {
  if (!node::Buffer::HasInstance(value)) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(
            isolate, "getInto output must be a Buffer")));
    return false;
  }
  *data = reinterpret_cast<unsigned char*>(node::Buffer::Data(value));
  *length = node::Buffer::Length(value);
  return true;
}

bool read_uint32(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    uint32_t* result,
    const char* name) {
  if (!value->IsUint32()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8(isolate, name).ToLocalChecked()));
    return false;
  }
  *result = value->Uint32Value(isolate->GetCurrentContext()).FromMaybe(0);
  return true;
}

bool read_size(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    size_t* result) {
  if (!value->IsNumber()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "slab size must be a number")));
    return false;
  }
  double number =
      value->NumberValue(isolate->GetCurrentContext()).FromMaybe(-1);
  if (number < 0 ||
      number > static_cast<double>(std::numeric_limits<size_t>::max()) ||
      number != static_cast<double>(static_cast<size_t>(number))) {
    isolate->ThrowException(v8::Exception::RangeError(
        v8::String::NewFromUtf8Literal(
            isolate, "slab_size_per_shard must be an integer size_t")));
    return false;
  }
  *result = static_cast<size_t>(number);
  return true;
}

int32_t get_into(
    FastCache* cache,
    const unsigned char* key,
    size_t key_length,
    unsigned char* output,
    size_t output_length) {
  for (unsigned int attempt = 0; attempt < 8; attempt += 1) {
    uint64_t packed = caeneus_get(
        cache->handle, key, key_length, output, output_length);
    int32_t status = static_cast<int32_t>(
        static_cast<uint32_t>(packed >> 32));
    uint32_t length = static_cast<uint32_t>(packed);
    if (status == CAENEUS_MISS) {
      return kFastMiss;
    }
    if (status == CAENEUS_OK) {
      if (length > static_cast<uint32_t>(
                       std::numeric_limits<int32_t>::max())) {
        return kFastFallback;
      }
      return static_cast<int32_t>(length);
    }
    if (status == CAENEUS_ERR_SMALL_BUF && length == 0) {
      continue;
    }
    if (status == CAENEUS_ERR_SMALL_BUF) {
      return kFastFallback;
    }
    return kFastFallback;
  }
  return kFastFallback;
}

#if NODE_MODULE_VERSION < 136
int32_t fast_set(
    v8::Local<v8::Object> receiver,
    const v8::FastOneByteString& key,
    const v8::FastApiTypedArray<uint8_t>& value) {
  FastCache* cache = get_cache(receiver);
  if (cache == nullptr || cache->closed || cache->handle == nullptr) {
    return kFastFallback;
  }
  uint8_t* value_data = nullptr;
  if (!value.getStorageIfAligned(&value_data)) {
    return kFastFallback;
  }
  return caeneus_set(
      cache->handle,
      reinterpret_cast<const unsigned char*>(key.data),
      key.length,
      value_data,
      value.length());
}
#endif

int32_t fast_get(
    v8::Local<v8::Object> receiver,
    const v8::FastOneByteString& key) {
  FastCache* cache = get_cache(receiver);
  if (cache == nullptr || cache->closed || cache->handle == nullptr) {
    return kFastFallback;
  }
  return get_into(
      cache,
      reinterpret_cast<const unsigned char*>(key.data),
      key.length,
      cache->scratch.data(),
      cache->scratch.size());
}

#if NODE_MODULE_VERSION < 136
int32_t fast_get_into(
    v8::Local<v8::Object> receiver,
    const v8::FastOneByteString& key,
    const v8::FastApiTypedArray<uint8_t>& output) {
  FastCache* cache = get_cache(receiver);
  if (cache == nullptr || cache->closed || cache->handle == nullptr) {
    return kFastFallback;
  }
  uint8_t* output_data = nullptr;
  if (!output.getStorageIfAligned(&output_data)) {
    return kFastFallback;
  }
  return get_into(
      cache,
      reinterpret_cast<const unsigned char*>(key.data),
      key.length,
      output_data,
      output.length());
}
#endif

#if NODE_MODULE_VERSION >= 136
int32_t fast_set_value(
    v8::Local<v8::Object> receiver,
    const v8::FastOneByteString& key,
    v8::Local<v8::Value> value) {
  FastCache* cache = get_cache(receiver);
  if (cache == nullptr || cache->closed || cache->handle == nullptr ||
      !node::Buffer::HasInstance(value)) {
    return kFastFallback;
  }
  return caeneus_set(
      cache->handle,
      reinterpret_cast<const unsigned char*>(key.data),
      key.length,
      reinterpret_cast<const unsigned char*>(node::Buffer::Data(value)),
      node::Buffer::Length(value));
}

int32_t fast_get_into_value(
    v8::Local<v8::Object> receiver,
    const v8::FastOneByteString& key,
    v8::Local<v8::Value> output) {
  FastCache* cache = get_cache(receiver);
  if (cache == nullptr || cache->closed || cache->handle == nullptr ||
      !node::Buffer::HasInstance(output)) {
    return kFastFallback;
  }
  return get_into(
      cache,
      reinterpret_cast<const unsigned char*>(key.data),
      key.length,
      reinterpret_cast<unsigned char*>(node::Buffer::Data(output)),
      node::Buffer::Length(output));
}
#endif

void slow_set(const v8::FunctionCallbackInfo<v8::Value>& args) {
  FastCache* cache = nullptr;
  if (!require_cache(args, &cache) || args.Length() < 2) {
    if (args.Length() < 2 && !args.GetIsolate()->IsExecutionTerminating()) {
      args.GetIsolate()->ThrowException(v8::Exception::TypeError(
          v8::String::NewFromUtf8Literal(
              args.GetIsolate(), "set requires key and value")));
    }
    return;
  }

  ByteView key;
  ByteView value;
  if (!read_bytes(args.GetIsolate(), args[0], &cache->key_storage, &key) ||
      !read_bytes(args.GetIsolate(), args[1], &cache->value_storage, &value)) {
    return;
  }
  int status = caeneus_set(
      cache->handle, key.data, key.length, value.data, value.length);
  if (status != CAENEUS_OK) {
    args.GetIsolate()->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(args.GetIsolate(), "caeneus set failed")));
    return;
  }
  args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), status));
}

void slow_get(const v8::FunctionCallbackInfo<v8::Value>& args) {
  FastCache* cache = nullptr;
  if (!require_cache(args, &cache) || args.Length() < 1) {
    if (args.Length() < 1 && !args.GetIsolate()->IsExecutionTerminating()) {
      args.GetIsolate()->ThrowException(v8::Exception::TypeError(
          v8::String::NewFromUtf8Literal(
              args.GetIsolate(), "get requires a key")));
    }
    return;
  }

  ByteView key;
  if (!read_bytes(args.GetIsolate(), args[0], &cache->key_storage, &key)) {
    return;
  }
  int32_t result = get_into(
      cache,
      key.data,
      key.length,
      cache->scratch.data(),
      cache->scratch.size());
  if (result == kFastMiss) {
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), kFastMiss));
  } else if (result == kFastFallback) {
    args.GetIsolate()->ThrowException(v8::Exception::RangeError(
        v8::String::NewFromUtf8Literal(
            args.GetIsolate(), "caeneus value exceeds the scratch buffer")));
  } else {
    args.GetReturnValue().Set(
        v8::Integer::New(args.GetIsolate(), result));
  }
}

void slow_get_into(const v8::FunctionCallbackInfo<v8::Value>& args) {
  FastCache* cache = nullptr;
  if (!require_cache(args, &cache) || args.Length() < 2) {
    if (args.Length() < 2 && !args.GetIsolate()->IsExecutionTerminating()) {
      args.GetIsolate()->ThrowException(v8::Exception::TypeError(
          v8::String::NewFromUtf8Literal(
              args.GetIsolate(), "getInto requires a key and output")));
    }
    return;
  }

  ByteView key;
  if (!read_bytes(args.GetIsolate(), args[0], &cache->key_storage, &key)) {
    return;
  }
  unsigned char* output = nullptr;
  size_t output_length = 0;
  if (!read_buffer_output(
          args.GetIsolate(), args[1], &output, &output_length)) {
    return;
  }
  int32_t result =
      get_into(cache, key.data, key.length, output, output_length);
  if (result == kFastMiss) {
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), kFastMiss));
  } else if (result == kFastFallback) {
    args.GetIsolate()->ThrowException(v8::Exception::RangeError(
        v8::String::NewFromUtf8Literal(
            args.GetIsolate(), "getInto output buffer is too small")));
  } else {
    args.GetReturnValue().Set(
        v8::Integer::New(args.GetIsolate(), result));
  }
}

void scratch_buffer(const v8::FunctionCallbackInfo<v8::Value>& args) {
  FastCache* cache = nullptr;
  if (!require_cache(args, &cache)) {
    return;
  }
  args.GetReturnValue().Set(cache->scratch_buffer.Get(args.GetIsolate()));
}

void close_cache(const v8::FunctionCallbackInfo<v8::Value>& args) {
  FastCache* cache = nullptr;
  if (!require_cache(args, &cache)) {
    return;
  }
  cache->close();
}

void finalize_cache(const v8::WeakCallbackInfo<FastCache>& info) {
  FastCache* cache = info.GetParameter();
  if (cache == nullptr) {
    return;
  }
  cache->wrapper.Reset();
  cache->scratch_buffer.Reset();
  cache->close();
}

void free_scratch(char* /*data*/, void* hint) {
  auto* cache = static_cast<FastCache*>(hint);
  if (cache == nullptr) {
    return;
  }
  cache->wrapper.Reset();
  cache->scratch_buffer.Reset();
  cache->close();
  delete cache;
}

#if NODE_MODULE_VERSION < 136
v8::CFunctionInfo fast_set_info() {
  static const v8::CTypeInfo args[] = {
      v8::CTypeInfo(v8::CTypeInfo::Type::kV8Value),
      v8::CTypeInfo(v8::CTypeInfo::Type::kSeqOneByteString),
      v8::CTypeInfo(
          v8::CTypeInfo::Type::kUint8,
          v8::CTypeInfo::SequenceType::kIsTypedArray),
  };
  return v8::CFunctionInfo(
      v8::CTypeInfo(v8::CTypeInfo::Type::kInt32),
      3,
      args);
}
#endif

v8::CFunctionInfo fast_get_info() {
  static const v8::CTypeInfo args[] = {
      v8::CTypeInfo(v8::CTypeInfo::Type::kV8Value),
      v8::CTypeInfo(v8::CTypeInfo::Type::kSeqOneByteString),
  };
  return v8::CFunctionInfo(
      v8::CTypeInfo(v8::CTypeInfo::Type::kInt32),
      2,
      args);
}

#if NODE_MODULE_VERSION >= 136
v8::CFunctionInfo fast_value_info() {
  static const v8::CTypeInfo args[] = {
      v8::CTypeInfo(v8::CTypeInfo::Type::kV8Value),
      v8::CTypeInfo(v8::CTypeInfo::Type::kSeqOneByteString),
      v8::CTypeInfo(v8::CTypeInfo::Type::kV8Value),
  };
  return v8::CFunctionInfo(
      v8::CTypeInfo(v8::CTypeInfo::Type::kInt32),
      3,
      args);
}
#endif
#if NODE_MODULE_VERSION < 136
v8::CFunctionInfo fast_get_into_info() {
  static const v8::CTypeInfo args[] = {
      v8::CTypeInfo(v8::CTypeInfo::Type::kV8Value),
      v8::CTypeInfo(v8::CTypeInfo::Type::kSeqOneByteString),
      v8::CTypeInfo(
          v8::CTypeInfo::Type::kUint8,
          v8::CTypeInfo::SequenceType::kIsTypedArray),
  };
  return v8::CFunctionInfo(
      v8::CTypeInfo(v8::CTypeInfo::Type::kInt32),
      3,
      args);
}
#endif

#if NODE_MODULE_VERSION < 136
v8::CFunction fast_set_function() {
  static const v8::CFunctionInfo info = fast_set_info();
  return v8::CFunction(
      reinterpret_cast<const void*>(&fast_set), &info);
}

#endif

v8::CFunction fast_get_function() {
  static const v8::CFunctionInfo info = fast_get_info();
  return v8::CFunction(
      reinterpret_cast<const void*>(&fast_get), &info);
}

#if NODE_MODULE_VERSION >= 136
v8::CFunction fast_set_value_function() {
  static const v8::CFunctionInfo info = fast_value_info();
  return v8::CFunction(
      reinterpret_cast<const void*>(&fast_set_value), &info);
}

v8::CFunction fast_get_into_value_function() {
  static const v8::CFunctionInfo info = fast_value_info();
  return v8::CFunction(
      reinterpret_cast<const void*>(&fast_get_into_value), &info);
}
#endif

#if NODE_MODULE_VERSION < 136
v8::CFunction fast_get_into_function() {
  static const v8::CFunctionInfo info = fast_get_into_info();
  return v8::CFunction(
      reinterpret_cast<const void*>(&fast_get_into), &info);
}
#endif

v8::Local<v8::Object> create_cache_object(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    FastCache* cache) {
  v8::Local<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetInternalFieldCount(2);

#if NODE_MODULE_VERSION < 136
  static const v8::CFunction set_function = fast_set_function();
  static const v8::CFunction get_into_function = fast_get_into_function();
#else
  static const v8::CFunction set_value_function = fast_set_value_function();
  static const v8::CFunction get_into_value_function =
      fast_get_into_value_function();
#endif
  static const v8::CFunction get_function = fast_get_function();

#if NODE_MODULE_VERSION < 136
  v8::Local<v8::FunctionTemplate> set_template = v8::FunctionTemplate::New(
      isolate,
      slow_set,
      v8::Local<v8::Value>(),
      v8::Local<v8::Signature>(),
      2,
      v8::ConstructorBehavior::kThrow,
      v8::SideEffectType::kHasSideEffect,
      &set_function);
#else
  v8::Local<v8::FunctionTemplate> set_template = v8::FunctionTemplate::New(
      isolate,
      slow_set,
      v8::Local<v8::Value>(),
      v8::Local<v8::Signature>(),
      2,
      v8::ConstructorBehavior::kThrow,
      v8::SideEffectType::kHasSideEffect,
      &set_value_function);
#endif
  v8::Local<v8::FunctionTemplate> get_template = v8::FunctionTemplate::New(
      isolate,
      slow_get,
      v8::Local<v8::Value>(),
      v8::Local<v8::Signature>(),
      1,
      v8::ConstructorBehavior::kThrow,
      v8::SideEffectType::kHasSideEffect,
      &get_function);
#if NODE_MODULE_VERSION < 136
  v8::Local<v8::FunctionTemplate> get_into_template =
      v8::FunctionTemplate::New(
          isolate,
          slow_get_into,
          v8::Local<v8::Value>(),
          v8::Local<v8::Signature>(),
          2,
          v8::ConstructorBehavior::kThrow,
          v8::SideEffectType::kHasSideEffect,
          &get_into_function);
#else
  v8::Local<v8::FunctionTemplate> get_into_template =
      v8::FunctionTemplate::New(
          isolate,
          slow_get_into,
          v8::Local<v8::Value>(),
          v8::Local<v8::Signature>(),
          2,
          v8::ConstructorBehavior::kThrow,
          v8::SideEffectType::kHasSideEffect,
          &get_into_value_function);
#endif

  object_template->Set(
      isolate, "set", set_template);
  object_template->Set(
      isolate, "get", get_template);
  object_template->Set(
      isolate, "getInto", get_into_template);
  object_template->Set(
      isolate, "setSlow", v8::FunctionTemplate::New(isolate, slow_set));
  object_template->Set(
      isolate, "getSlow", v8::FunctionTemplate::New(isolate, slow_get));
  object_template->Set(
      isolate,
      "getIntoSlow",
      v8::FunctionTemplate::New(isolate, slow_get_into));
  object_template->Set(
      isolate, "scratch", v8::FunctionTemplate::New(isolate, scratch_buffer));
  object_template->Set(
      isolate, "close", v8::FunctionTemplate::New(isolate, close_cache));

  v8::Local<v8::Object> object =
      object_template->NewInstance(context).ToLocalChecked();
  object->SetAlignedPointerInInternalField(1, cache);
  cache->wrapper.Reset(isolate, object);
  cache->wrapper.SetWeak(cache, finalize_cache, v8::WeakCallbackType::kParameter);
  return object;
}

void create_cache(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  uint32_t num_shards = 64;
  uint32_t slots_per_shard = 1024;
  size_t slab_size_per_shard = 1024 * 1024;
  if (args.Length() > 0 &&
      !read_uint32(isolate, args[0], &num_shards, "num_shards must be uint32")) {
    return;
  }
  if (args.Length() > 1 &&
      !read_uint32(
          isolate, args[1], &slots_per_shard, "slots_per_shard must be uint32")) {
    return;
  }
  if (args.Length() > 2 &&
      !read_size(isolate, args[2], &slab_size_per_shard)) {
    return;
  }

  std::unique_ptr<FastCache> cache(new FastCache());
  cache->scratch.resize(kScratchSize);
  cache->handle =
      caeneus_init(num_shards, slots_per_shard, slab_size_per_shard);
  if (cache->handle == nullptr) {
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(
            isolate, "caeneus initialization failed")));
    return;
  }

  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> object =
      create_cache_object(isolate, context, cache.get());
  v8::MaybeLocal<v8::Object> maybe_scratch = node::Buffer::New(
      isolate,
      reinterpret_cast<char*>(cache->scratch.data()),
      cache->scratch.size(),
      free_scratch,
      cache.get());
  v8::Local<v8::Object> scratch;
  if (!maybe_scratch.ToLocal(&scratch)) {
    cache->close();
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(
            isolate, "scratch Buffer allocation failed")));
    return;
  }
  cache->scratch_buffer.Reset(isolate, scratch);
  args.GetReturnValue().Set(object);
  cache.release();
}

}  // namespace

NODE_MODULE_INIT() {
  v8::Isolate* isolate = context->GetIsolate();
  exports
      ->Set(
          context,
          v8::String::NewFromUtf8Literal(isolate, "createCache"),
          v8::FunctionTemplate::New(isolate, create_cache)
              ->GetFunction(context)
              .ToLocalChecked())
      .Check();
}
