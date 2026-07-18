#include <node_api.h>

#include <caeneus.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace {

constexpr size_t kDefaultScratchSize = 64 * 1024;
// Upper bound on the reusable scratch buffer.  Values larger than this must be
// retrieved with getInto() using a caller-managed Buffer.  Prevents a single
// large cached value from pinning megabytes of native memory via napi_ref.
constexpr size_t kMaxScratchBytes = 4 * 1024 * 1024; // 4 MiB

struct ByteView {
  const unsigned char* data = nullptr;
  size_t length = 0;
};

struct Cache {
  napi_env env = nullptr;
  void* handle = nullptr;
  napi_ref scratch_ref = nullptr;
  napi_ref view_ref = nullptr;
  size_t view_length = 0;
  unsigned char* scratch_data = nullptr;
  size_t scratch_size = 0;
  bool closed = false;
  std::string key_storage;
  std::string value_storage;

  void clear_view() {
    if (view_ref != nullptr) {
      napi_delete_reference(env, view_ref);
      view_ref = nullptr;
    }
    view_length = 0;
  }

  void close() {
    if (closed) {
      return;
    }
    closed = true;
    if (handle != nullptr) {
      caeneus_deinit(handle);
      handle = nullptr;
    }
    clear_view();
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
    napi_throw_type_error(
        env, nullptr, "key and value must be a string or Buffer");
    return false;
  }

  // Prefer Latin-1 for one-byte/ASCII strings (same bytes as UTF-8). Fall back
  // to UTF-8 when any Latin-1 byte is >= 128 so Unicode keys stay UTF-8.
  constexpr size_t kMinKeyCapacity = 64;
  size_t capacity = string_storage->size();
  if (capacity < kMinKeyCapacity) {
    capacity = kMinKeyCapacity;
  }
  for (unsigned int attempt = 0; attempt < 4; attempt++) {
    string_storage->resize(capacity + 1);
    size_t written = 0;
    if (!check_status(env,
                      napi_get_value_string_latin1(env, value,
                                                   string_storage->data(),
                                                   capacity + 1, &written),
                      "read string latin1")) {
      return false;
    }
    if (written >= capacity) {
      capacity = capacity * 2;
      continue;
    }

    string_storage->resize(written);
    bool ascii = true;
    for (size_t i = 0; i < written; i++) {
      if (static_cast<unsigned char>((*string_storage)[i]) >= 128) {
        ascii = false;
        break;
      }
    }
    if (ascii) {
      view->data = reinterpret_cast<const unsigned char*>(
          string_storage->empty() ? "" : string_storage->data());
      view->length = written;
      return true;
    }
    break;
  }

  // UTF-8 path for non-ASCII strings. Single-pass into reusable storage;
  // grow and retry only when the buffer fills (possible truncation).
  capacity = string_storage->size();
  if (capacity < kMinKeyCapacity) {
    capacity = kMinKeyCapacity;
  }
  for (unsigned int attempt = 0; attempt < 4; attempt++) {
    string_storage->resize(capacity + 1);
    size_t written = 0;
    if (!check_status(env,
                      napi_get_value_string_utf8(env, value,
                                                 string_storage->data(),
                                                 capacity + 1, &written),
                      "read string utf8")) {
      return false;
    }
    if (written < capacity) {
      string_storage->resize(written);
      view->data = reinterpret_cast<const unsigned char*>(
          string_storage->empty() ? "" : string_storage->data());
      view->length = written;
      return true;
    }
    capacity = capacity * 2;
  }

  napi_throw_error(env, nullptr, "string key/value is too large");
  return false;
}

bool scratch_view(Cache* cache, size_t length, napi_value* result) {
  if (cache->scratch_ref == nullptr || cache->scratch_data == nullptr ||
      length > cache->scratch_size) {
    napi_throw_error(cache->env, nullptr, "caeneus scratch buffer is unavailable");
    return false;
  }

  // Reuse the cached subarray when length matches so steady-state gets avoid
  // allocating a new Buffer wrapper.
  if (cache->view_ref != nullptr && cache->view_length == length) {
    return check_status(
        cache->env,
        napi_get_reference_value(cache->env, cache->view_ref, result),
        "get cached scratch view");
  }

  cache->clear_view();

  napi_value scratch = nullptr;
  if (!check_status(cache->env,
                    napi_get_reference_value(cache->env, cache->scratch_ref,
                                             &scratch),
                    "get scratch buffer")) {
    return false;
  }

  if (length == cache->scratch_size) {
    if (!check_status(cache->env,
                      napi_create_reference(cache->env, scratch, 1,
                                            &cache->view_ref),
                      "pin scratch view")) {
      return false;
    }
    cache->view_length = length;
    *result = scratch;
    return true;
  }

  napi_value subarray = nullptr;
  if (!check_status(cache->env,
                    napi_get_named_property(cache->env, scratch, "subarray",
                                            &subarray),
                    "get Buffer.subarray")) {
    return false;
  }

  napi_value argv[2] = {};
  if (!check_status(cache->env, napi_create_uint32(cache->env, 0, &argv[0]),
                    "create subarray start") ||
      !check_status(cache->env,
                    napi_create_uint32(cache->env,
                                       static_cast<uint32_t>(length), &argv[1]),
                    "create subarray end")) {
    return false;
  }

  napi_value view = nullptr;
  if (!check_status(cache->env,
                    napi_call_function(cache->env, scratch, subarray, 2, argv,
                                       &view),
                    "call Buffer.subarray")) {
    return false;
  }

  if (!check_status(cache->env,
                    napi_create_reference(cache->env, view, 1, &cache->view_ref),
                    "pin scratch view")) {
    return false;
  }
  cache->view_length = length;
  *result = view;
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

  cache->clear_view();
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
      if (!scratch_view(cache, length, &result)) {
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
    const size_t required = static_cast<size_t>(length);
    if (required > kMaxScratchBytes) {
      napi_throw_range_error(
          env, nullptr,
          "caeneus value exceeds the scratch-buffer cap; use getInto() with a "
          "caller-managed Buffer for large values");
      return nullptr;
    }
    if (required > cache->scratch_size &&
        !replace_scratch(cache, required)) {
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

NAPI_MODULE_INIT() {
  return init_napi(env, exports);
}
