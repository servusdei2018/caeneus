#include <node_api.h>

#include <caeneus.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace {

constexpr size_t kDefaultScratchSize = 64 * 1024;

struct ByteView {
  const unsigned char* data = nullptr;
  size_t length = 0;
};

struct Cache {
  napi_env env = nullptr;
  void* handle = nullptr;
  napi_ref scratch_ref = nullptr;
  unsigned char* scratch_data = nullptr;
  size_t scratch_size = 0;
  bool closed = false;
  std::string key_storage;
  std::string value_storage;

  void close() {
    if (closed) {
      return;
    }
    closed = true;
    if (handle != nullptr) {
      caeneus_deinit(handle);
      handle = nullptr;
    }
    if (scratch_ref != nullptr) {
      napi_delete_reference(env, scratch_ref);
      scratch_ref = nullptr;
    }
    scratch_data = nullptr;
    scratch_size = 0;
  }
};

int32_t result_code(uint64_t packed) {
  return static_cast<int32_t>(static_cast<uint32_t>(packed >> 32));
}

uint32_t result_length(uint64_t packed) {
  return static_cast<uint32_t>(packed);
}

bool check_status(napi_env env, napi_status status, const char* operation) {
  if (status == napi_ok) {
    return true;
  }
  const napi_extended_error_info* info = nullptr;
  napi_get_last_error_info(env, &info);
  const char* detail = info != nullptr && info->error_message != nullptr
                           ? info->error_message
                           : "unknown Node-API error";
  std::string message = std::string(operation) + ": " + detail;
  napi_throw_error(env, nullptr, message.c_str());
  return false;
}

bool get_cache(napi_env env, napi_value value, Cache** result) {
  void* data = nullptr;
  if (!check_status(env, napi_get_value_external(env, value, &data),
                    "read native cache")) {
    return false;
  }
  Cache* cache = static_cast<Cache*>(data);
  if (cache == nullptr || cache->closed || cache->handle == nullptr) {
    napi_throw_error(env, nullptr, "caeneus cache is closed");
    return false;
  }
  *result = cache;
  return true;
}

bool read_string_or_buffer(napi_env env, napi_value value,
                           std::string* string_storage, ByteView* view) {
  bool is_buffer = false;
  if (!check_status(env, napi_is_buffer(env, value, &is_buffer),
                    "inspect key/value")) {
    return false;
  }
  if (is_buffer) {
    void* data = nullptr;
    size_t length = 0;
    if (!check_status(env, napi_get_buffer_info(env, value, &data, &length),
                      "read Buffer")) {
      return false;
    }
    view->data = static_cast<const unsigned char*>(data);
    view->length = length;
    return true;
  }

  napi_valuetype type = napi_undefined;
  if (!check_status(env, napi_typeof(env, value, &type), "inspect value type")) {
    return false;
  }
  if (type != napi_string) {
    napi_throw_type_error(env, nullptr, "key must be a string and value must be a string or Buffer");
    return false;
  }

  size_t length = 0;
  if (!check_status(env, napi_get_value_string_utf8(env, value, nullptr, 0, &length),
                    "measure string")) {
    return false;
  }
  string_storage->resize(length + 1);
  char* destination = string_storage->data();
  size_t written = 0;
  if (!check_status(env, napi_get_value_string_utf8(
                            env, value, destination, length + 1, &written),
                    "read string")) {
    return false;
  }
  string_storage->resize(written);
  view->data = reinterpret_cast<const unsigned char*>(
      string_storage->empty() ? "" : string_storage->data());
  view->length = written;
  return true;
}

bool read_uint32(napi_env env, napi_value value, uint32_t* result,
                 const char* name) {
  if (!check_status(env, napi_get_value_uint32(env, value, result), name)) {
    return false;
  }
  return true;
}

bool read_slab_size(napi_env env, napi_value value, size_t* result) {
  double number = 0;
  if (!check_status(env, napi_get_value_double(env, value, &number),
                    "read slab size")) {
    return false;
  }
  if (number < 0 || number > static_cast<double>(std::numeric_limits<size_t>::max()) ||
      number != static_cast<double>(static_cast<size_t>(number))) {
    napi_throw_range_error(env, nullptr, "slab_size_per_shard must be an integer size_t");
    return false;
  }
  *result = static_cast<size_t>(number);
  return true;
}

bool replace_scratch(Cache* cache, size_t required) {
  napi_value scratch = nullptr;
  void* data = nullptr;
  if (!check_status(cache->env,
                    napi_create_buffer(cache->env, required, &data, &scratch),
                    "allocate scratch buffer")) {
    return false;
  }

  napi_ref reference = nullptr;
  if (!check_status(cache->env,
                    napi_create_reference(cache->env, scratch, 1, &reference),
                    "pin scratch buffer")) {
    return false;
  }

  if (cache->scratch_ref != nullptr) {
    napi_delete_reference(cache->env, cache->scratch_ref);
  }
  cache->scratch_ref = reference;
  cache->scratch_data = static_cast<unsigned char*>(data);
  cache->scratch_size = required;
  return true;
}

void finalize_cache(napi_env env, void* data, void* /*hint*/) {
  Cache* cache = static_cast<Cache*>(data);
  if (cache == nullptr) {
    return;
  }
  cache->env = env;
  cache->close();
  delete cache;
}

napi_value create_cache(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {};
  if (!check_status(env, napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr),
                    "read createCache arguments")) {
    return nullptr;
  }

  uint32_t num_shards = 64;
  uint32_t slots_per_shard = 1024;
  size_t slab_size_per_shard = 1024 * 1024;
  if (argc > 0 && !read_uint32(env, argv[0], &num_shards, "read num_shards")) {
    return nullptr;
  }
  if (argc > 1 &&
      !read_uint32(env, argv[1], &slots_per_shard, "read slots_per_shard")) {
    return nullptr;
  }
  if (argc > 2 && !read_slab_size(env, argv[2], &slab_size_per_shard)) {
    return nullptr;
  }

  Cache* cache = new Cache();
  cache->env = env;
  cache->handle = caeneus_init(
      num_shards, slots_per_shard, slab_size_per_shard);
  if (cache->handle == nullptr) {
    delete cache;
    napi_throw_error(env, nullptr, "caeneus initialization failed");
    return nullptr;
  }

  if (!replace_scratch(cache, kDefaultScratchSize)) {
    cache->close();
    delete cache;
    return nullptr;
  }

  napi_value external = nullptr;
  if (!check_status(env, napi_create_external(
                              env, cache, finalize_cache, nullptr, &external),
                    "create native cache")) {
    cache->close();
    delete cache;
    return nullptr;
  }
  return external;
}

napi_value close_cache(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  if (!check_status(env, napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr),
                    "read close arguments") ||
      argc < 1) {
    napi_throw_type_error(env, nullptr, "closeCache requires a cache");
    return nullptr;
  }

  Cache* cache = nullptr;
  if (!get_cache(env, argv[0], &cache)) {
    return nullptr;
  }
  cache->close();
  napi_value undefined = nullptr;
  if (!check_status(env, napi_get_undefined(env, &undefined),
                    "create close result")) {
    return nullptr;
  }
  return undefined;
}

napi_value set_value(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {};
  if (!check_status(env, napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr),
                    "read set arguments") ||
      argc < 3) {
    napi_throw_type_error(env, nullptr, "set requires cache, key, and value");
    return nullptr;
  }

  Cache* cache = nullptr;
  if (!get_cache(env, argv[0], &cache)) {
    return nullptr;
  }
  ByteView key;
  ByteView value;
  if (!read_string_or_buffer(env, argv[1], &cache->key_storage, &key) ||
      !read_string_or_buffer(env, argv[2], &cache->value_storage, &value)) {
    return nullptr;
  }

  int status = caeneus_set(
      cache->handle,
      key.data,
      key.length,
      value.data,
      value.length);
  if (status != CAENEUS_OK) {
    napi_throw_error(env, nullptr, "caeneus set failed");
    return nullptr;
  }
  napi_value undefined = nullptr;
  if (!check_status(env, napi_get_undefined(env, &undefined),
                    "create set result")) {
    return nullptr;
  }
  return undefined;
}

napi_value get_value(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {};
  if (!check_status(env, napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr),
                    "read get arguments") ||
      argc < 2) {
    napi_throw_type_error(env, nullptr, "get requires cache and key");
    return nullptr;
  }

  Cache* cache = nullptr;
  if (!get_cache(env, argv[0], &cache)) {
    return nullptr;
  }
  ByteView key;
  if (!read_string_or_buffer(env, argv[1], &cache->key_storage, &key)) {
    return nullptr;
  }

  unsigned char probe = 0;
  for (unsigned int attempt = 0; attempt < 8; attempt++) {
    uint64_t packed = caeneus_get(
        cache->handle,
        key.data,
        key.length,
        cache->scratch_data != nullptr ? cache->scratch_data : &probe,
        cache->scratch_size);
    int32_t status = result_code(packed);
    uint32_t length = result_length(packed);

    if (status == CAENEUS_MISS) {
      napi_value null_value = nullptr;
      if (!check_status(env, napi_get_null(env, &null_value), "create cache miss")) {
        return nullptr;
      }
      return null_value;
    }
    if (status == CAENEUS_OK) {
      napi_value result = nullptr;
      if (!check_status(env, napi_create_uint32(env, length, &result),
                        "create result length")) {
        return nullptr;
      }
      return result;
    }
    if (status != CAENEUS_ERR_SMALL_BUF || length == 0) {
      napi_throw_error(env, nullptr, "caeneus returned an invalid get result");
      return nullptr;
    }
    if (static_cast<uint64_t>(length) >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      napi_throw_range_error(env, nullptr, "caeneus value is too large for size_t");
      return nullptr;
    }
    if (static_cast<size_t>(length) > cache->scratch_size &&
        !replace_scratch(cache, static_cast<size_t>(length))) {
      return nullptr;
    }
  }

  napi_throw_error(env, nullptr, "caeneus get could not stabilize during concurrent updates");
  return nullptr;
}

napi_value get_into_value(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {};
  if (!check_status(env, napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr),
                    "read getInto arguments") ||
      argc < 3) {
    napi_throw_type_error(env, nullptr, "getInto requires cache, key, and output");
    return nullptr;
  }

  Cache* cache = nullptr;
  if (!get_cache(env, argv[0], &cache)) {
    return nullptr;
  }
  ByteView key;
  if (!read_string_or_buffer(env, argv[1], &cache->key_storage, &key)) {
    return nullptr;
  }

  bool is_buffer = false;
  if (!check_status(env, napi_is_buffer(env, argv[2], &is_buffer),
                    "inspect getInto output")) {
    return nullptr;
  }
  if (!is_buffer) {
    napi_throw_type_error(env, nullptr, "getInto output must be a Buffer");
    return nullptr;
  }

  void* output_data = nullptr;
  size_t output_size = 0;
  if (!check_status(env, napi_get_buffer_info(
                            env, argv[2], &output_data, &output_size),
                    "read getInto output")) {
    return nullptr;
  }

  for (unsigned int attempt = 0; attempt < 8; attempt++) {
    uint64_t packed = caeneus_get(
        cache->handle,
        key.data,
        key.length,
        static_cast<unsigned char*>(output_data),
        output_size);
    int32_t status = result_code(packed);
    uint32_t length = result_length(packed);

    if (status == CAENEUS_MISS) {
      napi_value null_value = nullptr;
      if (!check_status(env, napi_get_null(env, &null_value),
                        "create getInto miss")) {
        return nullptr;
      }
      return null_value;
    }
    if (status == CAENEUS_OK) {
      napi_value result = nullptr;
      if (!check_status(env, napi_create_uint32(env, length, &result),
                        "create getInto result length")) {
        return nullptr;
      }
      return result;
    }
    if (status == CAENEUS_ERR_SMALL_BUF && length == 0) {
      continue;
    }
    if (status == CAENEUS_ERR_SMALL_BUF) {
      napi_throw_range_error(
          env, nullptr, "getInto output buffer is too small");
      return nullptr;
    }
    napi_throw_error(env, nullptr, "caeneus getInto failed");
    return nullptr;
  }

  napi_throw_error(
      env, nullptr, "caeneus getInto could not stabilize during concurrent updates");
  return nullptr;
}

napi_value scratch_buffer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  if (!check_status(env, napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr),
                    "read scratch arguments") ||
      argc < 1) {
    napi_throw_type_error(env, nullptr, "scratch requires a cache");
    return nullptr;
  }

  Cache* cache = nullptr;
  if (!get_cache(env, argv[0], &cache)) {
    return nullptr;
  }
  napi_value scratch = nullptr;
  if (!check_status(env, napi_get_reference_value(
                              env, cache->scratch_ref, &scratch),
                    "read scratch buffer")) {
    return nullptr;
  }
  return scratch;
}

napi_value fast_api_available(napi_env env, napi_callback_info /*info*/) {
  napi_value result = nullptr;
#if defined(CAENEUS_ENABLE_V8_FAST_API) && CAENEUS_ENABLE_V8_FAST_API
  constexpr bool enabled = true;
#else
  constexpr bool enabled = false;
#endif
  if (!check_status(env, napi_get_boolean(env, enabled, &result),
                    "create fast API flag")) {
    return nullptr;
  }
  return result;
}

napi_value init_napi(napi_env env, napi_value exports) {
  napi_property_descriptor properties[] = {
      {"createCache", nullptr, create_cache, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"closeCache", nullptr, close_cache, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"set", nullptr, set_value, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"get", nullptr, get_value, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getInto", nullptr, get_into_value, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"scratch", nullptr, scratch_buffer, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"fastApiAvailable", nullptr, fast_api_available, nullptr, nullptr, nullptr,
       napi_default, nullptr},
  };
  if (!check_status(env, napi_define_properties(
                            env,
                            exports,
                            sizeof(properties) / sizeof(properties[0]),
                            properties),
                    "register Node-API methods")) {
    return nullptr;
  }
  return exports;
}

}  // namespace

/*
 * The stable build uses Node-API only. V8 Fast API Calls are deliberately
 * isolated behind CAENEUS_ENABLE_V8_FAST_API because they require direct V8
 * registration and are not part of Node-API's ABI-stable surface. The
 * opt-in branch below provides the secondary flat typed-array path; normal
 * TypeScript code always uses the portable Node-API entry points above.
 */
#if defined(CAENEUS_ENABLE_V8_FAST_API) && CAENEUS_ENABLE_V8_FAST_API

#include <node.h>
#include <v8.h>
#include <v8-fast-api-calls.h>

namespace {

struct FastCache {
  void* handle = nullptr;
};

void cleanup_fast_cache(void* data) {
  auto* cache = static_cast<FastCache*>(data);
  if (cache == nullptr) {
    return;
  }
  if (cache->handle != nullptr) {
    caeneus_deinit(cache->handle);
    cache->handle = nullptr;
  }
  delete cache;
}

int32_t fast_set(
    v8::Local<v8::Value> receiver_value,
    const v8::FastOneByteString& key,
    const v8::FastApiTypedArray<uint8_t>& value) {
  v8::Local<v8::Object> receiver_object = receiver_value.As<v8::Object>();
  auto* receiver = static_cast<FastCache*>(
      receiver_object->GetAlignedPointerFromInternalField(1));
  if (receiver == nullptr || receiver->handle == nullptr) {
    return CAENEUS_ERR_PANIC;
  }
  uint8_t* value_data = nullptr;
  if (!value.getStorageIfAligned(&value_data)) {
    return CAENEUS_ERR_PANIC;
  }
  return caeneus_set(
      receiver->handle,
      reinterpret_cast<const unsigned char*>(key.data),
      key.length,
      value_data,
      value.length());
}

uint64_t fast_get(
    v8::Local<v8::Value> receiver_value,
    const v8::FastOneByteString& key,
    const v8::FastApiTypedArray<uint8_t>& output) {
  v8::Local<v8::Object> receiver_object = receiver_value.As<v8::Object>();
  auto* receiver = static_cast<FastCache*>(
      receiver_object->GetAlignedPointerFromInternalField(1));
  if (receiver == nullptr || receiver->handle == nullptr) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(CAENEUS_ERR_PANIC)) << 32);
  }
  uint8_t* output_data = nullptr;
  if (!output.getStorageIfAligned(&output_data)) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(CAENEUS_ERR_PANIC)) << 32);
  }
  return caeneus_get(
      receiver->handle,
      reinterpret_cast<const unsigned char*>(key.data),
      key.length,
      output_data,
      output.length());
}

void slow_set(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  auto* receiver = static_cast<FastCache*>(
      args.This()->GetAlignedPointerFromInternalField(1));
  if (receiver == nullptr || args.Length() < 2 ||
      !args[0]->IsString() || !args[1]->IsUint8Array()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "fast.set expects a string and Uint8Array")));
    return;
  }

  v8::String::Utf8Value key(isolate, args[0]);
  auto value = args[1].As<v8::Uint8Array>();
  std::shared_ptr<v8::BackingStore> backing = value->GetBackingStore();
  int32_t status = caeneus_set(
      receiver->handle,
      reinterpret_cast<const unsigned char*>(*key),
      key.length() < 0 ? 0 : static_cast<size_t>(key.length()),
      static_cast<const unsigned char*>(backing->Data()) + value->ByteOffset(),
      value->ByteLength());
  if (status != CAENEUS_OK) {
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "caeneus fast set failed")));
    return;
  }
}

void slow_get(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  auto* receiver = static_cast<FastCache*>(
      args.This()->GetAlignedPointerFromInternalField(1));
  if (receiver == nullptr || args.Length() < 2 ||
      !args[0]->IsString() || !args[1]->IsUint8Array()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "fast.get expects a string and Uint8Array")));
    return;
  }

  v8::String::Utf8Value key(isolate, args[0]);
  auto output = args[1].As<v8::Uint8Array>();
  std::shared_ptr<v8::BackingStore> backing = output->GetBackingStore();
  uint64_t packed = caeneus_get(
      receiver->handle,
      reinterpret_cast<const unsigned char*>(*key),
      key.length() < 0 ? 0 : static_cast<size_t>(key.length()),
      static_cast<unsigned char*>(backing->Data()) + output->ByteOffset(),
      output->ByteLength());
  args.GetReturnValue().Set(v8::BigInt::NewFromUnsigned(isolate, packed));
}

void install_fast_api(
    v8::Isolate* isolate,
    v8::Local<v8::Object> exports,
    v8::Local<v8::Context> context,
    FastCache* cache) {
  v8::Local<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  constexpr int kEmbedderWrapperTypeIndex = 0;
  constexpr int kEmbedderWrapperObjectIndex = 1;
  isolate->set_embedder_wrapper_type_index(kEmbedderWrapperTypeIndex);
  isolate->set_embedder_wrapper_object_index(kEmbedderWrapperObjectIndex);
  object_template->SetInternalFieldCount(kEmbedderWrapperObjectIndex + 1);

  v8::CFunction set_c_function = v8::CFunction::Make(fast_set);
  v8::CFunction get_c_function = v8::CFunction::Make(fast_get);
  v8::Local<v8::FunctionTemplate> set_template =
      v8::FunctionTemplate::New(
          isolate,
          slow_set,
          v8::Local<v8::Value>(),
          v8::Local<v8::Signature>(),
          2,
          v8::ConstructorBehavior::kThrow,
          v8::SideEffectType::kHasSideEffect,
          &set_c_function);
  v8::Local<v8::FunctionTemplate> get_template =
      v8::FunctionTemplate::New(
          isolate,
          slow_get,
          v8::Local<v8::Value>(),
          v8::Local<v8::Signature>(),
          2,
          v8::ConstructorBehavior::kThrow,
          v8::SideEffectType::kHasSideEffect,
          &get_c_function);

  v8::Local<v8::Object> fast_object =
      object_template->NewInstance(context).ToLocalChecked();
  fast_object->SetAlignedPointerInInternalField(kEmbedderWrapperObjectIndex, cache);
  fast_object
      ->Set(context, v8::String::NewFromUtf8Literal(isolate, "set"),
            set_template->GetFunction(context).ToLocalChecked())
      .Check();
  fast_object
      ->Set(context, v8::String::NewFromUtf8Literal(isolate, "get"),
            get_template->GetFunction(context).ToLocalChecked())
      .Check();
  exports
      ->Set(context, v8::String::NewFromUtf8Literal(isolate, "fast"),
            fast_object)
      .Check();
}

}  // namespace

NODE_MODULE_INIT(/* exports, module, context */) {
  v8::Isolate* isolate = context->GetIsolate();
  auto* cache = new FastCache();
  cache->handle = caeneus_init(64, 1024, 1024 * 1024);
  if (cache->handle == nullptr) {
    delete cache;
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "caeneus initialization failed")));
    return;
  }
  node::AddEnvironmentCleanupHook(isolate, cleanup_fast_cache, cache);
  install_fast_api(isolate, exports, context, cache);
}

#else

NAPI_MODULE_INIT() {
  return init_napi(env, exports);
}

#endif
