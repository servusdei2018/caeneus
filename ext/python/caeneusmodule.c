#define PY_SSIZE_T_CLEAN

#include <Python.h>

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdatomic.h>

#include "caeneus.h"

#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL _Thread_local
#endif

static THREAD_LOCAL int my_thread_slot = -1;
#define FREELIST_SLOTS 64
#define FREELIST_DEPTH 2
#define MAX_FREELIST_CAPACITY 65536

static THREAD_LOCAL PyObject *my_freelist[FREELIST_DEPTH] = {NULL, NULL};
static THREAD_LOCAL Py_ssize_t my_freelist_capacity[FREELIST_DEPTH] = {0, 0};
static THREAD_LOCAL int my_next_freelist_idx = 0;
static THREAD_LOCAL Py_ssize_t my_get_capacity = 0;

static inline const char *
get_ascii_key(PyObject *op, Py_ssize_t *length) {
    if (PyUnicode_IS_COMPACT_ASCII(op)) {
        *length = ((PyASCIIObject *)op)->length;
        return (const char *)((PyASCIIObject *)op + 1);
    }
    return PyUnicode_AsUTF8AndSize(op, length);
}

typedef struct {
    PyObject_HEAD
    void *handle;
    _Atomic unsigned int active_calls;
    unsigned int gil_probe;
    int concurrent_mode;
    /* GIL-protected initial capacity for one-copy small-value reads. */
    Py_ssize_t get_capacity;
    /* Thread-hashed freelists to avoid contention and leaks */
    PyObject *get_freelist[FREELIST_SLOTS][FREELIST_DEPTH];
    Py_ssize_t freelist_capacity[FREELIST_SLOTS][FREELIST_DEPTH];
    Py_ssize_t gil_threshold;
} CaeneusCache;

static inline int32_t result_code(uint64_t packed) {
    return (int32_t)(uint32_t)(packed >> 32);
}

static inline uint32_t result_length(uint64_t packed) {
    return (uint32_t)packed;
}

/*
 * get_immutable_bytes returns a borrowed, immutable byte view.
 *
 * Unicode objects expose their cached UTF-8 representation; bytes objects
 * expose their storage directly. Keeping the owning Python objects referenced
 * by the caller makes both pointers safe while the GIL is released for the
 * native operation.
 */
typedef struct {
    Py_buffer view;
    int has_view;
} RawBuffer;

#define ACQUIRE_RAW_BUFFER_FAST(object, data, length, raw_buf, on_error) \
    do { \
        raw_buf.has_view = 0; \
        if (PyUnicode_CheckExact(object)) { \
            Py_ssize_t unicode_length = 0; \
            const char *utf8 = PyUnicode_AsUTF8AndSize(object, &unicode_length); \
            if (utf8 == NULL) { on_error; } \
            data = (const unsigned char *)utf8; \
            length = unicode_length; \
        } else if (PyBytes_CheckExact(object)) { \
            data = (const unsigned char *)PyBytes_AS_STRING(object); \
            length = PyBytes_GET_SIZE(object); \
        } else if (PyUnicode_Check(object)) { \
            Py_ssize_t unicode_length = 0; \
            const char *utf8 = PyUnicode_AsUTF8AndSize(object, &unicode_length); \
            if (utf8 == NULL) { on_error; } \
            data = (const unsigned char *)utf8; \
            length = unicode_length; \
        } else if (PyBytes_Check(object)) { \
            data = (const unsigned char *)PyBytes_AS_STRING(object); \
            length = PyBytes_GET_SIZE(object); \
        } else { \
            if (PyObject_GetBuffer(object, &raw_buf.view, PyBUF_SIMPLE) == 0) { \
                data = (const unsigned char *)raw_buf.view.buf; \
                length = raw_buf.view.len; \
                raw_buf.has_view = 1; \
            } else { \
                PyErr_SetString( \
                    PyExc_TypeError, \
                    "key and value must support the buffer protocol, or be str/bytes"); \
                on_error; \
            } \
        } \
    } while (0)

static inline void
release_raw_buffer(RawBuffer *buf) {
    if (buf->has_view) {
        PyBuffer_Release(&buf->view);
    }
}

static inline int
get_open_handle(CaeneusCache *self, void **handle) {
    *handle = self->handle;
    if (*handle != NULL) {
        return 0;
    }
    PyErr_SetString(PyExc_RuntimeError, "caeneus cache is closed");
    return -1;
}

static inline int
begin_native_call(CaeneusCache *self, void **handle) {
    if (get_open_handle(self, handle) < 0) {
        return -1;
    }
    atomic_fetch_add_explicit(&self->active_calls, 1, memory_order_acquire);
    return 0;
}

static inline void
end_native_call(CaeneusCache *self) {
    atomic_fetch_sub_explicit(&self->active_calls, 1, memory_order_release);
}

static void
wait_for_native_calls(CaeneusCache *self) {
    Py_BEGIN_ALLOW_THREADS
    while (atomic_load_explicit(&self->active_calls, memory_order_acquire) != 0) {
    }
    Py_END_ALLOW_THREADS
}

static inline int
should_release_gil(CaeneusCache *self) {
    if (self->concurrent_mode) {
        return 1;
    }
    if (atomic_load_explicit(&self->active_calls, memory_order_relaxed) != 0) {
        self->concurrent_mode = 1;
        return 1;
    }
    if (--self->gil_probe == 0) {
        self->gil_probe = 1024;
        return 1;
    }
    return 0;
}

static inline int
native_set(CaeneusCache *self,
           const unsigned char *key_data,
           Py_ssize_t key_length,
           const unsigned char *value_data,
           Py_ssize_t value_length,
           int *status) {
    if (value_length <= self->gil_threshold) {
        void *handle = self->handle;
        if (handle == NULL) {
            return -1;
        }
        *status = caeneus_set(
            handle,
            key_data,
            (size_t)key_length,
            value_data,
            (size_t)value_length);
        return 0;
    }

    int release_gil = should_release_gil(self);

    if (!release_gil) {
        void *handle = self->handle;
        if (handle == NULL) {
            return -1;
        }
        *status = caeneus_set(
            handle,
            key_data,
            (size_t)key_length,
            value_data,
            (size_t)value_length);
        return 0;
    }

    void *handle = NULL;
    if (begin_native_call(self, &handle) < 0) {
        return -1;
    }

    Py_BEGIN_ALLOW_THREADS
    *status = caeneus_set(
        handle,
        key_data,
        (size_t)key_length,
        value_data,
        (size_t)value_length);
    Py_END_ALLOW_THREADS

    end_native_call(self);
    return 0;
}



static void
clear_get_freelist(CaeneusCache *self) {
    for (int s = 0; s < FREELIST_SLOTS; s++) {
        for (int d = 0; d < FREELIST_DEPTH; d++) {
            Py_XDECREF(self->get_freelist[s][d]);
            self->get_freelist[s][d] = NULL;
            self->freelist_capacity[s][d] = 0;
        }
    }
}

static inline __attribute__((always_inline)) PyObject *
acquire_get_output(CaeneusCache *self, unsigned int slot, Py_ssize_t capacity) {
    (void)self;
    (void)slot;
    for (int i = 0; i < FREELIST_DEPTH; i++) {
        PyObject *output = my_freelist[i];
        if (output != NULL &&
            Py_REFCNT(output) == 1 &&
            my_freelist_capacity[i] >= capacity) {
            my_freelist[i] = NULL;
            Py_SET_SIZE((PyVarObject *)output, my_freelist_capacity[i]);
            ((char *)PyBytes_AS_STRING(output))[my_freelist_capacity[i]] = '\0';
            return output;
        }
    }
    return PyBytes_FromStringAndSize(NULL, capacity);
}

static inline __attribute__((always_inline)) void
store_get_freelist(CaeneusCache *self, unsigned int slot, PyObject *output, Py_ssize_t capacity) {
    (void)self;
    (void)slot;
    if (capacity > MAX_FREELIST_CAPACITY) {
        return;
    }
    for (int i = 0; i < FREELIST_DEPTH; i++) {
        if (my_freelist[i] == NULL) {
            my_freelist[i] = output;
            Py_INCREF(output);
            my_freelist_capacity[i] = capacity;
            return;
        }
    }
    for (int i = 0; i < FREELIST_DEPTH; i++) {
        PyObject *old = my_freelist[i];
        if (old != NULL && Py_REFCNT(old) == 1) {
            Py_DECREF(old);
            my_freelist[i] = output;
            Py_INCREF(output);
            my_freelist_capacity[i] = capacity;
            return;
        }
    }
}

static inline __attribute__((always_inline)) void
abandon_get_output(CaeneusCache *self, unsigned int slot, PyObject *output, Py_ssize_t capacity) {
    (void)self;
    (void)slot;
    if (output == NULL) {
        return;
    }
    if (capacity > MAX_FREELIST_CAPACITY) {
        Py_DECREF(output);
        return;
    }
    for (int i = 0; i < FREELIST_DEPTH; i++) {
        if (my_freelist[i] == NULL && Py_REFCNT(output) == 1) {
            my_freelist[i] = output;
            my_freelist_capacity[i] = capacity;
            return;
        }
    }
    Py_DECREF(output);
}

static inline int
native_get(CaeneusCache *self,
           const unsigned char *key_data,
           Py_ssize_t key_length,
           unsigned char *buffer,
           Py_ssize_t buffer_length,
           uint64_t *packed) {
    if (buffer_length <= self->gil_threshold) {
        void *handle = self->handle;
        if (handle == NULL) {
            return -1;
        }
        *packed = caeneus_get(
            handle,
            key_data,
            (size_t)key_length,
            buffer,
            (size_t)buffer_length);
        return 0;
    }

    int release_gil = 0;
    if (!self->concurrent_mode) {
        if (atomic_load_explicit(&self->active_calls, memory_order_relaxed) != 0) {
            self->concurrent_mode = 1;
        }
    }
    release_gil = self->concurrent_mode
        ? 1
        : (buffer_length > 1024 && should_release_gil(self));

    if (!release_gil) {
        void *handle = self->handle;
        if (handle == NULL) {
            return -1;
        }
        *packed = caeneus_get(
            handle,
            key_data,
            (size_t)key_length,
            buffer,
            (size_t)buffer_length);
        return 0;
    }

    void *handle = NULL;
    if (begin_native_call(self, &handle) < 0) {
        return -1;
    }

    Py_BEGIN_ALLOW_THREADS
    *packed = caeneus_get(
        handle,
        key_data,
        (size_t)key_length,
        buffer,
        (size_t)buffer_length);
    Py_END_ALLOW_THREADS

    end_native_call(self);
    return 0;
}

static int
cache_init(CaeneusCache *self, PyObject *args, PyObject *kwargs) {
    static char *keywords[] = {
        "num_shards",
        "slots_per_shard",
        "slab_size_per_shard",
        "initial_value_capacity",
        "gil_threshold",
        NULL,
    };
    unsigned int num_shards = 64;
    unsigned int slots_per_shard = 1024;
    unsigned long long slab_size_per_shard = 1024ULL * 1024ULL;
    unsigned long long initial_value_capacity = 128;
    long long gil_threshold = 1024;

    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "|IIKKL:Cache",
            keywords,
            &num_shards,
            &slots_per_shard,
            &slab_size_per_shard,
            &initial_value_capacity,
            &gil_threshold)) {
        return -1;
    }

    if (slab_size_per_shard > (unsigned long long)SIZE_MAX) {
        PyErr_SetString(PyExc_OverflowError, "slab size does not fit size_t");
        return -1;
    }
    if (initial_value_capacity > (unsigned long long)PY_SSIZE_T_MAX) {
        PyErr_SetString(
            PyExc_OverflowError,
            "initial value capacity does not fit a Python bytes object");
        return -1;
    }
    if (gil_threshold < 0) {
        PyErr_SetString(PyExc_ValueError, "gil_threshold cannot be negative");
        return -1;
    }

    if (self->handle != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "caeneus cache is already initialized");
        return -1;
    }

    atomic_init(&self->active_calls, 0);
    self->gil_probe = 1024;
    self->concurrent_mode = 0;
    self->gil_threshold = (Py_ssize_t)gil_threshold;
    for (int s = 0; s < FREELIST_SLOTS; s++) {
        for (int d = 0; d < FREELIST_DEPTH; d++) {
            self->get_freelist[s][d] = NULL;
            self->freelist_capacity[s][d] = 0;
        }
    }

    void *handle = NULL;
    Py_BEGIN_ALLOW_THREADS
    handle = caeneus_init(
        num_shards,
        slots_per_shard,
        (size_t)slab_size_per_shard);
    Py_END_ALLOW_THREADS

    if (handle == NULL) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "caeneus initialization failed; check shard, slot, and slab settings");
        return -1;
    }

    self->handle = handle;
    self->get_capacity = (Py_ssize_t)initial_value_capacity;
    return 0;
}

static void
cache_dealloc(CaeneusCache *self) {
    clear_get_freelist(self);
    if (self->handle != NULL) {
        void *handle = self->handle;
        self->handle = NULL;
        self->get_capacity = 0;
        wait_for_native_calls(self);
        Py_BEGIN_ALLOW_THREADS
        caeneus_deinit(handle);
        Py_END_ALLOW_THREADS
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
cache_close(CaeneusCache *self, PyObject *Py_UNUSED(ignored)) {
    clear_get_freelist(self);
    if (self->handle != NULL) {
        void *handle = self->handle;
        self->handle = NULL;
        self->get_capacity = 0;
        wait_for_native_calls(self);
        Py_BEGIN_ALLOW_THREADS
        caeneus_deinit(handle);
        Py_END_ALLOW_THREADS
    }
    Py_RETURN_NONE;
}

static inline int
cache_set_impl(CaeneusCache *self, PyObject *key_object, PyObject *value_object) {
    void *handle = self->handle;
    if (__builtin_expect(handle == NULL, 0)) {
        PyErr_SetString(PyExc_RuntimeError, "caeneus cache is closed");
        return -1;
    }

    if (__builtin_expect(PyUnicode_CheckExact(key_object) && PyBytes_CheckExact(value_object), 1)) {
        if (__builtin_expect(PyUnicode_IS_COMPACT_ASCII(key_object), 1)) {
            Py_ssize_t unicode_length = ((PyASCIIObject *)key_object)->length;
            const char *utf8 = (const char *)((PyASCIIObject *)key_object + 1);
            Py_ssize_t value_len = PyBytes_GET_SIZE(value_object);
            if (__builtin_expect(value_len <= self->gil_threshold, 1)) {
                int status = caeneus_set(
                    handle,
                    (const unsigned char *)utf8,
                    (size_t)unicode_length,
                    (const unsigned char *)PyBytes_AS_STRING(value_object),
                    (size_t)value_len);
                if (__builtin_expect(status != CAENEUS_OK, 0)) {
                    PyErr_SetString(PyExc_RuntimeError, "caeneus set failed");
                    return -1;
                }
                return 0;
            }
        }
    }

    const unsigned char *key_data = NULL;
    Py_ssize_t key_length = 0;
    const unsigned char *value_data = NULL;
    Py_ssize_t value_length = 0;

    int key_has_view = 0;
    int val_has_view = 0;
    Py_buffer key_view;
    Py_buffer val_view;

    if (PyUnicode_CheckExact(key_object)) {
        Py_ssize_t unicode_length = 0;
        const char *utf8 = get_ascii_key(key_object, &unicode_length);
        if (utf8 == NULL) return -1;
        key_data = (const unsigned char *)utf8;
        key_length = unicode_length;
    } else if (PyBytes_CheckExact(key_object)) {
        key_data = (const unsigned char *)PyBytes_AS_STRING(key_object);
        key_length = PyBytes_GET_SIZE(key_object);
    } else {
        if (PyUnicode_Check(key_object)) {
            Py_ssize_t unicode_length = 0;
            const char *utf8 = get_ascii_key(key_object, &unicode_length);
            if (utf8 == NULL) return -1;
            key_data = (const unsigned char *)utf8;
            key_length = unicode_length;
        } else if (PyBytes_Check(key_object)) {
            key_data = (const unsigned char *)PyBytes_AS_STRING(key_object);
            key_length = PyBytes_GET_SIZE(key_object);
        } else {
            if (PyObject_GetBuffer(key_object, &key_view, PyBUF_SIMPLE) == 0) {
                key_data = (const unsigned char *)key_view.buf;
                key_length = key_view.len;
                key_has_view = 1;
            } else {
                PyErr_SetString(PyExc_TypeError, "key must support the buffer protocol, or be str/bytes");
                return -1;
            }
        }
    }

    if (PyBytes_CheckExact(value_object)) {
        value_data = (const unsigned char *)PyBytes_AS_STRING(value_object);
        value_length = PyBytes_GET_SIZE(value_object);
    } else if (PyUnicode_CheckExact(value_object)) {
        Py_ssize_t unicode_length = 0;
        const char *utf8 = get_ascii_key(value_object, &unicode_length);
        if (utf8 == NULL) {
            if (key_has_view) PyBuffer_Release(&key_view);
            return -1;
        }
        value_data = (const unsigned char *)utf8;
        value_length = unicode_length;
    } else {
        if (PyBytes_Check(value_object)) {
            value_data = (const unsigned char *)PyBytes_AS_STRING(value_object);
            value_length = PyBytes_GET_SIZE(value_object);
        } else if (PyUnicode_Check(value_object)) {
            Py_ssize_t unicode_length = 0;
            const char *utf8 = get_ascii_key(value_object, &unicode_length);
            if (utf8 == NULL) {
                if (key_has_view) PyBuffer_Release(&key_view);
                return -1;
            }
            value_data = (const unsigned char *)utf8;
            value_length = unicode_length;
        } else {
            if (PyObject_GetBuffer(value_object, &val_view, PyBUF_SIMPLE) == 0) {
                value_data = (const unsigned char *)val_view.buf;
                value_length = val_view.len;
                val_has_view = 1;
            } else {
                if (key_has_view) PyBuffer_Release(&key_view);
                PyErr_SetString(PyExc_TypeError, "value must support the buffer protocol, or be str/bytes");
                return -1;
            }
        }
    }

    int status = CAENEUS_ERR_PANIC;
    int ret = 0;
    if (native_set(
            self,
            key_data,
            key_length,
            value_data,
            value_length,
            &status) < 0) {
        ret = -1;
        goto cleanup;
    }

    if (status != CAENEUS_OK) {
        PyErr_SetString(PyExc_RuntimeError, "caeneus set failed");
        ret = -1;
    }

cleanup:
    if (val_has_view) PyBuffer_Release(&val_view);
    if (key_has_view) PyBuffer_Release(&key_view);
    return ret;
}

static PyObject *
cache_set(CaeneusCache *self, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs != 2) {
        PyErr_Format(
            PyExc_TypeError,
            "set() takes exactly 2 arguments (%zd given)",
            nargs);
        return NULL;
    }
    if (cache_set_impl(self, args[0], args[1]) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static inline PyObject *
cache_get_impl(CaeneusCache *self, PyObject *key_object, int is_subscript) {
    void *open_handle = self->handle;
    if (__builtin_expect(open_handle == NULL, 0)) {
        PyErr_SetString(PyExc_RuntimeError, "caeneus cache is closed");
        return NULL;
    }

    if (__builtin_expect(PyUnicode_CheckExact(key_object), 1)) {
        if (__builtin_expect(PyUnicode_IS_COMPACT_ASCII(key_object), 1)) {
            Py_ssize_t unicode_length = ((PyASCIIObject *)key_object)->length;
            const char *utf8 = (const char *)((PyASCIIObject *)key_object + 1);
            int idx = my_next_freelist_idx;
            PyObject *output = my_freelist[idx];
            if (__builtin_expect(output != NULL && Py_REFCNT(output) == 1 && my_freelist_capacity[idx] >= my_get_capacity, 1)) {
                my_next_freelist_idx = idx ^ 1;
                uint64_t packed = caeneus_get(
                    open_handle,
                    (const unsigned char *)utf8,
                    (size_t)unicode_length,
                    (unsigned char *)PyBytes_AS_STRING(output),
                    (size_t)my_freelist_capacity[idx]);
                
                int32_t status = result_code(packed);
                uint32_t required_length = result_length(packed);
                
                if (__builtin_expect(status == CAENEUS_OK && (Py_ssize_t)required_length <= my_freelist_capacity[idx], 1)) {
                    if (__builtin_expect((Py_ssize_t)required_length < my_freelist_capacity[idx], 0)) {
                        Py_SET_SIZE((PyVarObject *)output, (Py_ssize_t)required_length);
                        ((char *)PyBytes_AS_STRING(output))[required_length] = '\0';
                    }
                    my_get_capacity = (Py_ssize_t)required_length;
                    Py_INCREF(output);
                    return output;
                } else if (status == CAENEUS_MISS) {
                    if (is_subscript) {
                        PyErr_SetObject(PyExc_KeyError, key_object);
                        return NULL;
                    }
                    Py_RETURN_NONE;
                } else if (status == CAENEUS_ERR_PANIC) {
                    PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
                    return NULL;
                }
            } else {
                idx ^= 1;
                output = my_freelist[idx];
                if (__builtin_expect(output != NULL && Py_REFCNT(output) == 1 && my_freelist_capacity[idx] >= my_get_capacity, 1)) {
                    my_next_freelist_idx = idx ^ 1;
                    uint64_t packed = caeneus_get(
                        open_handle,
                        (const unsigned char *)utf8,
                        (size_t)unicode_length,
                        (unsigned char *)PyBytes_AS_STRING(output),
                        (size_t)my_freelist_capacity[idx]);
                    
                    int32_t status = result_code(packed);
                    uint32_t required_length = result_length(packed);
                    
                    if (__builtin_expect(status == CAENEUS_OK && (Py_ssize_t)required_length <= my_freelist_capacity[idx], 1)) {
                        if (__builtin_expect((Py_ssize_t)required_length < my_freelist_capacity[idx], 0)) {
                            Py_SET_SIZE((PyVarObject *)output, (Py_ssize_t)required_length);
                            ((char *)PyBytes_AS_STRING(output))[required_length] = '\0';
                        }
                        my_get_capacity = (Py_ssize_t)required_length;
                        Py_INCREF(output);
                        return output;
                    } else if (status == CAENEUS_MISS) {
                        if (is_subscript) {
                            PyErr_SetObject(PyExc_KeyError, key_object);
                            return NULL;
                        }
                        Py_RETURN_NONE;
                    } else if (status == CAENEUS_ERR_PANIC) {
                        PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
                        return NULL;
                    }
                }
            }
            goto do_unicode_get;
        }
    }

    const unsigned char *key_data = NULL;
    Py_ssize_t key_length = 0;
    int key_has_view = 0;
    Py_buffer key_view;

    if (PyBytes_CheckExact(key_object)) {
        key_data = (const unsigned char *)PyBytes_AS_STRING(key_object);
        key_length = PyBytes_GET_SIZE(key_object);
    } else {
        if (PyUnicode_Check(key_object)) {
            Py_ssize_t unicode_length = 0;
            const char *utf8 = get_ascii_key(key_object, &unicode_length);
            if (utf8 == NULL) return NULL;
            key_data = (const unsigned char *)utf8;
            key_length = unicode_length;
        } else if (PyBytes_Check(key_object)) {
            key_data = (const unsigned char *)PyBytes_AS_STRING(key_object);
            key_length = PyBytes_GET_SIZE(key_object);
        } else {
            if (PyObject_GetBuffer(key_object, &key_view, PyBUF_SIMPLE) == 0) {
                key_data = (const unsigned char *)key_view.buf;
                key_length = key_view.len;
                key_has_view = 1;
            } else {
                PyErr_SetString(
                    PyExc_TypeError,
                    "key must support the buffer protocol, or be str/bytes");
                return NULL;
            }
        }
    }

    // Alternating freelist lookup for generic keys
    int idx = my_next_freelist_idx;
    PyObject *output = my_freelist[idx];
    if (output != NULL && Py_REFCNT(output) == 1 && my_freelist_capacity[idx] >= my_get_capacity) {
        my_next_freelist_idx = idx ^ 1;
        uint64_t packed = caeneus_get(
            open_handle,
            key_data,
            (size_t)key_length,
            (unsigned char *)PyBytes_AS_STRING(output),
            (size_t)my_freelist_capacity[idx]);
        
        int32_t status = result_code(packed);
        uint32_t required_length = result_length(packed);
        
        if (status == CAENEUS_OK) {
            if ((Py_ssize_t)required_length <= my_freelist_capacity[idx]) {
                if ((Py_ssize_t)required_length < my_freelist_capacity[idx]) {
                    Py_SET_SIZE((PyVarObject *)output, (Py_ssize_t)required_length);
                    ((char *)PyBytes_AS_STRING(output))[required_length] = '\0';
                }
                my_get_capacity = (Py_ssize_t)required_length;
                Py_INCREF(output);
                if (key_has_view) PyBuffer_Release(&key_view);
                return output;
            }
        } else if (status == CAENEUS_MISS) {
            if (key_has_view) PyBuffer_Release(&key_view);
            if (is_subscript) {
                PyErr_SetObject(PyExc_KeyError, key_object);
                return NULL;
            }
            Py_RETURN_NONE;
        } else if (status == CAENEUS_ERR_PANIC) {
            if (key_has_view) PyBuffer_Release(&key_view);
            PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
            return NULL;
        }
    } else {
        idx ^= 1;
        output = my_freelist[idx];
        if (output != NULL && Py_REFCNT(output) == 1 && my_freelist_capacity[idx] >= my_get_capacity) {
            my_next_freelist_idx = idx ^ 1;
            uint64_t packed = caeneus_get(
                open_handle,
                key_data,
                (size_t)key_length,
                (unsigned char *)PyBytes_AS_STRING(output),
                (size_t)my_freelist_capacity[idx]);
            
            int32_t status = result_code(packed);
            uint32_t required_length = result_length(packed);
            
            if (status == CAENEUS_OK) {
                if ((Py_ssize_t)required_length <= my_freelist_capacity[idx]) {
                    if ((Py_ssize_t)required_length < my_freelist_capacity[idx]) {
                        Py_SET_SIZE((PyVarObject *)output, (Py_ssize_t)required_length);
                        ((char *)PyBytes_AS_STRING(output))[required_length] = '\0';
                    }
                    my_get_capacity = (Py_ssize_t)required_length;
                    Py_INCREF(output);
                    if (key_has_view) PyBuffer_Release(&key_view);
                    return output;
                }
            } else if (status == CAENEUS_MISS) {
                if (key_has_view) PyBuffer_Release(&key_view);
                if (is_subscript) {
                    PyErr_SetObject(PyExc_KeyError, key_object);
                    return NULL;
                }
                Py_RETURN_NONE;
            } else if (status == CAENEUS_ERR_PANIC) {
                if (key_has_view) PyBuffer_Release(&key_view);
                PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
                return NULL;
            }
        }
    }

    /* Fallback path: generic robust lookup */
    unsigned char stack_buf[1024];
    output = NULL;
    Py_ssize_t output_capacity = my_get_capacity;
    uint64_t packed = 0;

    if (output_capacity > 0) {
        output = acquire_get_output(self, 0, output_capacity);
        if (output == NULL) {
            if (key_has_view) PyBuffer_Release(&key_view);
            return NULL;
        }
        if (native_get(
                self,
                key_data,
                key_length,
                (unsigned char *)PyBytes_AS_STRING(output),
                PyBytes_GET_SIZE(output),
                &packed) < 0) {
            abandon_get_output(self, 0, output, PyBytes_GET_SIZE(output));
            if (key_has_view) PyBuffer_Release(&key_view);
            return NULL;
        }
        output_capacity = PyBytes_GET_SIZE(output);
    } else {
        if (native_get(
                self,
                key_data,
                key_length,
                stack_buf,
                sizeof(stack_buf),
                &packed) < 0) {
            if (key_has_view) PyBuffer_Release(&key_view);
            return NULL;
        }
    }

    int32_t status = result_code(packed);
    uint32_t required_length = result_length(packed);

    if (status == CAENEUS_MISS) {
        abandon_get_output(self, 0, output, output_capacity);
        if (key_has_view) PyBuffer_Release(&key_view);
        if (is_subscript) {
            PyErr_SetObject(PyExc_KeyError, key_object);
            return NULL;
        }
        Py_RETURN_NONE;
    }
    if (status == CAENEUS_ERR_PANIC) {
        abandon_get_output(self, 0, output, output_capacity);
        if (key_has_view) PyBuffer_Release(&key_view);
        PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
        return NULL;
    }
    if (status == CAENEUS_OK) {
        if (output != NULL) {
            if ((uint64_t)required_length > (uint64_t)output_capacity) {
                abandon_get_output(self, 0, output, output_capacity);
                if (key_has_view) PyBuffer_Release(&key_view);
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "caeneus returned a value larger than its output buffer");
                return NULL;
            }
            if ((Py_ssize_t)required_length < output_capacity) {
                unsigned char *output_data =
                    (unsigned char *)PyBytes_AS_STRING(output);
                output_data[required_length] = '\0';
                Py_SET_SIZE((PyVarObject *)output, (Py_ssize_t)required_length);
            }
            my_get_capacity = (Py_ssize_t)required_length;
            store_get_freelist(self, 0, output, output_capacity);
            if (key_has_view) PyBuffer_Release(&key_view);
            return output;
        }
        my_get_capacity = (Py_ssize_t)required_length;
        if (key_has_view) PyBuffer_Release(&key_view);
        return PyBytes_FromStringAndSize(
            (const char *)stack_buf,
            (Py_ssize_t)required_length);
    }

    // Fallback: value is larger than the pre-allocated buffer
    abandon_get_output(self, 0, output, output_capacity);
    output = NULL;
    output_capacity = (Py_ssize_t)required_length;

    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        if (output_capacity == 0) {
            unsigned char probe = 0;
            if (native_get(
                    self,
                    key_data,
                    key_length,
                    &probe,
                    0,
                    &packed) < 0) {
                if (key_has_view) PyBuffer_Release(&key_view);
                return NULL;
            }
            int32_t status = result_code(packed);
            uint32_t required_length = result_length(packed);
            if (status == CAENEUS_MISS) {
                if (key_has_view) PyBuffer_Release(&key_view);
                if (is_subscript) {
                    PyErr_SetObject(PyExc_KeyError, key_object);
                    return NULL;
                }
                Py_RETURN_NONE;
            }
            if (status == CAENEUS_ERR_PANIC) {
                if (key_has_view) PyBuffer_Release(&key_view);
                PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
                return NULL;
            }
            if (status == CAENEUS_ERR_SMALL_BUF) {
                output_capacity = (Py_ssize_t)required_length;
                continue;
            }
            if (required_length == 0) {
                my_get_capacity = 0;
                if (key_has_view) PyBuffer_Release(&key_view);
                return PyBytes_FromStringAndSize("", 0);
            }
            if (key_has_view) PyBuffer_Release(&key_view);
            PyErr_SetString(PyExc_RuntimeError, "caeneus returned success for a zero-byte probe");
            return NULL;
        }

        if ((uint64_t)output_capacity > (uint64_t)PY_SSIZE_T_MAX) {
            if (key_has_view) PyBuffer_Release(&key_view);
            PyErr_SetString(PyExc_OverflowError, "caeneus value is too large for a Python bytes object");
            return NULL;
        }

        output = acquire_get_output(self, 0, output_capacity);
        if (output == NULL) {
            if (key_has_view) PyBuffer_Release(&key_view);
            return NULL;
        }
        Py_ssize_t owned_capacity = PyBytes_GET_SIZE(output);

        if (native_get(
                self,
                key_data,
                key_length,
                (unsigned char *)PyBytes_AS_STRING(output),
                owned_capacity,
                &packed) < 0) {
            abandon_get_output(self, 0, output, owned_capacity);
            if (key_has_view) PyBuffer_Release(&key_view);
            return NULL;
        }

        status = result_code(packed);
        required_length = result_length(packed);

        if (status == CAENEUS_MISS) {
            abandon_get_output(self, 0, output, owned_capacity);
            if (key_has_view) PyBuffer_Release(&key_view);
            if (is_subscript) {
                PyErr_SetObject(PyExc_KeyError, key_object);
                return NULL;
            }
            Py_RETURN_NONE;
        }
        if (status == CAENEUS_ERR_PANIC) {
            abandon_get_output(self, 0, output, owned_capacity);
            if (key_has_view) PyBuffer_Release(&key_view);
            PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
            return NULL;
        }
        if (status == CAENEUS_OK) {
            if ((uint64_t)required_length > (uint64_t)owned_capacity) {
                abandon_get_output(self, 0, output, owned_capacity);
                if (key_has_view) PyBuffer_Release(&key_view);
                PyErr_SetString(PyExc_RuntimeError, "caeneus returned a value larger than its output buffer");
                return NULL;
            }
            if ((Py_ssize_t)required_length < owned_capacity) {
                unsigned char *output_data = (unsigned char *)PyBytes_AS_STRING(output);
                output_data[required_length] = '\0';
                Py_SET_SIZE((PyVarObject *)output, (Py_ssize_t)required_length);
            }
            my_get_capacity = (Py_ssize_t)required_length;
            store_get_freelist(self, 0, output, owned_capacity);
            if (key_has_view) PyBuffer_Release(&key_view);
            return output;
        }

        abandon_get_output(self, 0, output, owned_capacity);
        output_capacity = (Py_ssize_t)required_length;
    }

    if (key_has_view) PyBuffer_Release(&key_view);
    PyErr_SetString(
        PyExc_RuntimeError,
        "caeneus get could not stabilize during concurrent updates");
    return NULL;

do_unicode_get: {
    unsigned char stack_buf[1024];
    PyObject *output = NULL;
    Py_ssize_t output_capacity = my_get_capacity;
    uint64_t packed = 0;

    Py_ssize_t unicode_length = 0;
    const char *utf8 = get_ascii_key(key_object, &unicode_length);

    if (output_capacity > 0) {
        output = acquire_get_output(self, 0, output_capacity);
        if (output == NULL) return NULL;
        if (native_get(
                self,
                (const unsigned char *)utf8,
                unicode_length,
                (unsigned char *)PyBytes_AS_STRING(output),
                PyBytes_GET_SIZE(output),
                &packed) < 0) {
            abandon_get_output(self, 0, output, PyBytes_GET_SIZE(output));
            return NULL;
        }
        output_capacity = PyBytes_GET_SIZE(output);
    } else {
        if (native_get(
                self,
                (const unsigned char *)utf8,
                unicode_length,
                stack_buf,
                sizeof(stack_buf),
                &packed) < 0) {
            return NULL;
        }
    }

    int32_t status = result_code(packed);
    uint32_t required_length = result_length(packed);

    if (status == CAENEUS_MISS) {
        abandon_get_output(self, 0, output, output_capacity);
        if (is_subscript) {
            PyErr_SetObject(PyExc_KeyError, key_object);
            return NULL;
        }
        Py_RETURN_NONE;
    }
    if (status == CAENEUS_ERR_PANIC) {
        abandon_get_output(self, 0, output, output_capacity);
        PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
        return NULL;
    }
    if (status == CAENEUS_OK) {
        if (output != NULL) {
            if ((uint64_t)required_length > (uint64_t)output_capacity) {
                abandon_get_output(self, 0, output, output_capacity);
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "caeneus returned a value larger than its output buffer");
                return NULL;
            }
            if ((Py_ssize_t)required_length < output_capacity) {
                unsigned char *output_data =
                    (unsigned char *)PyBytes_AS_STRING(output);
                output_data[required_length] = '\0';
                Py_SET_SIZE((PyVarObject *)output, (Py_ssize_t)required_length);
            }
            my_get_capacity = (Py_ssize_t)required_length;
            store_get_freelist(self, 0, output, output_capacity);
            return output;
        }
        my_get_capacity = (Py_ssize_t)required_length;
        return PyBytes_FromStringAndSize(
            (const char *)stack_buf,
            (Py_ssize_t)required_length);
    }

    abandon_get_output(self, 0, output, output_capacity);
    output = NULL;
    output_capacity = (Py_ssize_t)required_length;

    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        if (output_capacity == 0) {
            unsigned char probe = 0;
            if (native_get(
                    self,
                    (const unsigned char *)utf8,
                    unicode_length,
                    &probe,
                    0,
                    &packed) < 0) {
                return NULL;
            }
            int32_t status = result_code(packed);
            uint32_t required_length = result_length(packed);
            if (status == CAENEUS_MISS) {
                if (is_subscript) {
                    PyErr_SetObject(PyExc_KeyError, key_object);
                    return NULL;
                }
                Py_RETURN_NONE;
            }
            if (status == CAENEUS_ERR_PANIC) {
                PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
                return NULL;
            }
            if (status == CAENEUS_ERR_SMALL_BUF) {
                output_capacity = (Py_ssize_t)required_length;
                continue;
            }
            if (required_length == 0) {
                my_get_capacity = 0;
                return PyBytes_FromStringAndSize("", 0);
            }
            PyErr_SetString(PyExc_RuntimeError, "caeneus returned success for a zero-byte probe");
            return NULL;
        }

        if ((uint64_t)output_capacity > (uint64_t)PY_SSIZE_T_MAX) {
            PyErr_SetString(PyExc_OverflowError, "caeneus value is too large for a Python bytes object");
            return NULL;
        }

        output = acquire_get_output(self, 0, output_capacity);
        if (output == NULL) return NULL;
        Py_ssize_t owned_capacity = PyBytes_GET_SIZE(output);

        if (native_get(
                self,
                (const unsigned char *)utf8,
                unicode_length,
                (unsigned char *)PyBytes_AS_STRING(output),
                owned_capacity,
                &packed) < 0) {
            abandon_get_output(self, 0, output, owned_capacity);
            return NULL;
        }

        status = result_code(packed);
        required_length = result_length(packed);

        if (status == CAENEUS_MISS) {
            abandon_get_output(self, 0, output, owned_capacity);
            if (is_subscript) {
                PyErr_SetObject(PyExc_KeyError, key_object);
                return NULL;
            }
            Py_RETURN_NONE;
        }
        if (status == CAENEUS_ERR_PANIC) {
            abandon_get_output(self, 0, output, owned_capacity);
            PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
            return NULL;
        }
        if (status == CAENEUS_OK) {
            if ((uint64_t)required_length > (uint64_t)owned_capacity) {
                abandon_get_output(self, 0, output, owned_capacity);
                PyErr_SetString(PyExc_RuntimeError, "caeneus returned a value larger than its output buffer");
                return NULL;
            }
            if ((Py_ssize_t)required_length < owned_capacity) {
                unsigned char *output_data = (unsigned char *)PyBytes_AS_STRING(output);
                output_data[required_length] = '\0';
                Py_SET_SIZE((PyVarObject *)output, (Py_ssize_t)required_length);
            }
            my_get_capacity = (Py_ssize_t)required_length;
            store_get_freelist(self, 0, output, owned_capacity);
            return output;
        }

        abandon_get_output(self, 0, output, owned_capacity);
        output_capacity = (Py_ssize_t)required_length;
    }

    PyErr_SetString(
        PyExc_RuntimeError,
        "caeneus get could not stabilize during concurrent updates");
    return NULL;
}
}

static PyObject *
cache_get(CaeneusCache *self, PyObject *arg) {
    return cache_get_impl(self, arg, 0);
}

static PyObject *
cache_subscript(CaeneusCache *self, PyObject *key) {
    return cache_get_impl(self, key, 1);
}

static int
cache_ass_subscript(CaeneusCache *self, PyObject *key, PyObject *value) {
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "deleting keys is not supported");
        return -1;
    }
    return cache_set_impl(self, key, value);
}
static PyObject *
cache_get_into(CaeneusCache *self, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs != 2) {
        PyErr_Format(
            PyExc_TypeError,
            "get_into() takes exactly 2 arguments (%zd given)",
            nargs);
        return NULL;
    }

    void *open_handle = self->handle;
    if (__builtin_expect(open_handle == NULL, 0)) {
        PyErr_SetString(PyExc_RuntimeError, "caeneus cache is closed");
        return NULL;
    }

    PyObject *key_object = args[0];
    PyObject *target = args[1];

    if (__builtin_expect(PyUnicode_CheckExact(key_object), 1)) {
        if (__builtin_expect(PyUnicode_IS_COMPACT_ASCII(key_object), 1)) {
            Py_ssize_t unicode_length = ((PyASCIIObject *)key_object)->length;
            const char *utf8 = (const char *)((PyASCIIObject *)key_object + 1);
            unsigned char *buf_ptr = NULL;
            Py_ssize_t buf_len = 0;
            int is_generic_buffer = 0;
            Py_buffer output_view;

            if (PyByteArray_Check(target)) {
                buf_ptr = (unsigned char *)PyByteArray_AS_STRING(target);
                buf_len = PyByteArray_GET_SIZE(target);
            } else {
                if (PyObject_GetBuffer(
                        target,
                        &output_view,
                        PyBUF_WRITABLE | PyBUF_C_CONTIGUOUS) < 0) {
                    return NULL;
                }
                buf_ptr = (unsigned char *)output_view.buf;
                buf_len = output_view.len;
                is_generic_buffer = 1;
            }

            for (unsigned int attempt = 0; attempt < 8; attempt++) {
                uint64_t packed = 0;
                if (native_get(
                        self,
                        (const unsigned char *)utf8,
                        unicode_length,
                        buf_ptr,
                        buf_len,
                        &packed) < 0) {
                    if (is_generic_buffer) PyBuffer_Release(&output_view);
                    return NULL;
                }

                int32_t status = result_code(packed);
                uint32_t output_length = result_length(packed);
                if (status == CAENEUS_MISS) {
                    if (is_generic_buffer) PyBuffer_Release(&output_view);
                    Py_RETURN_NONE;
                }
                if (status == CAENEUS_OK) {
                    if ((uint64_t)output_length > (uint64_t)buf_len) {
                        if (is_generic_buffer) PyBuffer_Release(&output_view);
                        PyErr_SetString(
                            PyExc_RuntimeError,
                            "caeneus returned a value larger than its output buffer");
                        return NULL;
                    }
                    if (is_generic_buffer) PyBuffer_Release(&output_view);
                    return PyLong_FromUnsignedLong((unsigned long)output_length);
                }
                if (status == CAENEUS_ERR_SMALL_BUF) {
                    if (output_length == 0) {
                        continue;
                    }
                    if (is_generic_buffer) PyBuffer_Release(&output_view);
                    PyErr_Format(
                        PyExc_BufferError,
                        "caeneus value requires %u bytes",
                        output_length);
                    return NULL;
                }

                if (is_generic_buffer) PyBuffer_Release(&output_view);
                PyErr_SetString(PyExc_RuntimeError, "caeneus get_into failed");
                return NULL;
            }

            if (is_generic_buffer) PyBuffer_Release(&output_view);
            PyErr_SetString(
                PyExc_RuntimeError,
                "caeneus get_into could not stabilize during concurrent updates");
            return NULL;
        }
    }

    const unsigned char *key_data = NULL;
    Py_ssize_t key_length = 0;
    int key_has_view = 0;
    Py_buffer key_view;

    if (PyBytes_CheckExact(key_object)) {
        key_data = (const unsigned char *)PyBytes_AS_STRING(key_object);
        key_length = PyBytes_GET_SIZE(key_object);
    } else {
        if (PyUnicode_Check(key_object)) {
            Py_ssize_t unicode_length = 0;
            const char *utf8 = get_ascii_key(key_object, &unicode_length);
            if (utf8 == NULL) return NULL;
            key_data = (const unsigned char *)utf8;
            key_length = unicode_length;
        } else if (PyBytes_Check(key_object)) {
            key_data = (const unsigned char *)PyBytes_AS_STRING(key_object);
            key_length = PyBytes_GET_SIZE(key_object);
        } else {
            if (PyObject_GetBuffer(key_object, &key_view, PyBUF_SIMPLE) == 0) {
                key_data = (const unsigned char *)key_view.buf;
                key_length = key_view.len;
                key_has_view = 1;
            } else {
                PyErr_SetString(
                    PyExc_TypeError,
                    "key must support the buffer protocol, or be str/bytes");
                return NULL;
            }
        }
    }

    unsigned char *buf_ptr = NULL;
    Py_ssize_t buf_len = 0;
    int is_generic_buffer = 0;
    Py_buffer output_view;

    if (PyByteArray_Check(target)) {
        buf_ptr = (unsigned char *)PyByteArray_AS_STRING(target);
        buf_len = PyByteArray_GET_SIZE(target);
    } else {
        if (PyObject_GetBuffer(
                target,
                &output_view,
                PyBUF_WRITABLE | PyBUF_C_CONTIGUOUS) < 0) {
            if (key_has_view) PyBuffer_Release(&key_view);
            return NULL;
        }
        buf_ptr = (unsigned char *)output_view.buf;
        buf_len = output_view.len;
        is_generic_buffer = 1;
    }

    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        uint64_t packed = 0;
        if (native_get(
                self,
                key_data,
                key_length,
                buf_ptr,
                buf_len,
                &packed) < 0) {
            if (is_generic_buffer) PyBuffer_Release(&output_view);
            if (key_has_view) PyBuffer_Release(&key_view);
            return NULL;
        }

        int32_t status = result_code(packed);
        uint32_t output_length = result_length(packed);
        if (status == CAENEUS_MISS) {
            if (is_generic_buffer) PyBuffer_Release(&output_view);
            if (key_has_view) PyBuffer_Release(&key_view);
            Py_RETURN_NONE;
        }
        if (status == CAENEUS_OK) {
            if ((uint64_t)output_length > (uint64_t)buf_len) {
                if (is_generic_buffer) PyBuffer_Release(&output_view);
                if (key_has_view) PyBuffer_Release(&key_view);
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "caeneus returned a value larger than its output buffer");
                return NULL;
            }
            if (is_generic_buffer) PyBuffer_Release(&output_view);
            if (key_has_view) PyBuffer_Release(&key_view);
            return PyLong_FromUnsignedLong((unsigned long)output_length);
        }
        if (status == CAENEUS_ERR_SMALL_BUF) {
            if (output_length == 0) {
                continue;
            }
            if (is_generic_buffer) PyBuffer_Release(&output_view);
            if (key_has_view) PyBuffer_Release(&key_view);
            PyErr_Format(
                PyExc_BufferError,
                "caeneus value requires %u bytes",
                output_length);
            return NULL;
        }

        if (is_generic_buffer) PyBuffer_Release(&output_view);
        if (key_has_view) PyBuffer_Release(&key_view);
        PyErr_SetString(PyExc_RuntimeError, "caeneus get_into failed");
        return NULL;
    }

    if (is_generic_buffer) PyBuffer_Release(&output_view);
    if (key_has_view) PyBuffer_Release(&key_view);
    PyErr_SetString(
        PyExc_RuntimeError,
        "caeneus get_into could not stabilize during concurrent updates");
    return NULL;
}

static PyMethodDef cache_methods[] = {
    {
        "set",
        (PyCFunction)(void(*)(void))cache_set,
        METH_FASTCALL,
        "set() stores a str or bytes value under a str or bytes key.\n"
        "\n"
        "The cache does not retain either input object.",
    },
    {
        "get",
        (PyCFunction)cache_get,
        METH_O,
        "get() retrieves the value for a str or bytes key.\n"
        "\n"
        "It returns a new bytes object, or None if the key is not in the cache.",
    },
    {
        "get_into",
        (PyCFunction)(void(*)(void))cache_get_into,
        METH_FASTCALL,
        "get_into() retrieves a value into a writable contiguous buffer.\n"
        "\n"
        "It returns the number of bytes written, or None on a cache miss. The\n"
        "buffer must be large enough for the value.",
    },
    {
        "close",
        (PyCFunction)cache_close,
        METH_NOARGS,
        "close() closes the cache and releases its native resources.\n"
        "\n"
        "Calling close() more than once is safe. After close(), the Cache "
        "cannot be used.",
    },
    {NULL, NULL, 0, NULL},
};

static PyMappingMethods cache_as_mapping = {
    .mp_length = NULL,
    .mp_subscript = (binaryfunc)cache_subscript,
    .mp_ass_subscript = (objobjargproc)cache_ass_subscript,
};

static PyTypeObject CaeneusCacheType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "caeneus.Cache",
    .tp_doc =
        "Cache is a native Caeneus in-memory cache client.\n"
        "\n"
        "Cache operations are safe for concurrent use by multiple Python "
        "threads.\n"
        "\n"
        "num_shards must be a power of two. slots_per_shard must be at least "
        "64. slab_size_per_shard must be at least the system page size.\n"
        "\n"
        "initial_value_capacity optionally seeds the read buffer size "
        "(default 128); zero uses a size probe on the first read.",
    .tp_basicsize = sizeof(CaeneusCache),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = cache_methods,
    .tp_as_mapping = &cache_as_mapping,
    .tp_init = (initproc)cache_init,
    .tp_new = PyType_GenericNew,
    .tp_dealloc = (destructor)cache_dealloc,
};

static PyObject *
cache_vectorcall(PyObject *type, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);

    unsigned int num_shards = 64;
    unsigned int slots_per_shard = 1024;
    unsigned long long slab_size_per_shard = 1024ULL * 1024ULL;
    unsigned long long initial_value_capacity = 128;
    long long gil_threshold = 1024;

    for (Py_ssize_t i = 0; i < nargs; i++) {
        PyObject *val = args[i];
        if (i == 0 || i == 1) {
            long long val_ll = PyLong_AsLongLong(val);
            if (val_ll == -1 && PyErr_Occurred()) {
                return NULL;
            }
            if (val_ll < 0 || val_ll > UINT_MAX) {
                PyErr_SetString(PyExc_ValueError, "argument must be a non-negative 32-bit integer");
                return NULL;
            }
            if (i == 0) {
                num_shards = (unsigned int)val_ll;
            } else {
                slots_per_shard = (unsigned int)val_ll;
            }
        } else if (i == 2 || i == 3) {
            unsigned long long val_ull = PyLong_AsUnsignedLongLong(val);
            if (val_ull == (unsigned long long)-1 && PyErr_Occurred()) {
                return NULL;
            }
            if (i == 2) {
                slab_size_per_shard = val_ull;
            } else {
                initial_value_capacity = val_ull;
            }
        } else if (i == 4) {
            long long val_ll = PyLong_AsLongLong(val);
            if (val_ll == -1 && PyErr_Occurred()) {
                return NULL;
            }
            if (val_ll < 0) {
                PyErr_SetString(PyExc_ValueError, "gil_threshold must be a non-negative integer");
                return NULL;
            }
            gil_threshold = val_ll;
        } else {
            PyErr_SetString(PyExc_TypeError, "Cache() takes at most 5 positional arguments");
            return NULL;
        }
    }

    for (Py_ssize_t i = 0; i < nkwargs; i++) {
        PyObject *key = PyTuple_GET_ITEM(kwnames, i);
        PyObject *val = args[nargs + i];
        const char *name = PyUnicode_AsUTF8(key);
        if (name == NULL) {
            return NULL;
        }

        if (strcmp(name, "num_shards") == 0 || strcmp(name, "slots_per_shard") == 0) {
            long long val_ll = PyLong_AsLongLong(val);
            if (val_ll == -1 && PyErr_Occurred()) {
                return NULL;
            }
            if (val_ll < 0 || val_ll > UINT_MAX) {
                PyErr_SetString(PyExc_ValueError, "argument must be a non-negative 32-bit integer");
                return NULL;
            }
            if (strcmp(name, "num_shards") == 0) {
                num_shards = (unsigned int)val_ll;
            } else {
                slots_per_shard = (unsigned int)val_ll;
            }
        } else if (strcmp(name, "slab_size_per_shard") == 0 || strcmp(name, "initial_value_capacity") == 0) {
            unsigned long long val_ull = PyLong_AsUnsignedLongLong(val);
            if (val_ull == (unsigned long long)-1 && PyErr_Occurred()) {
                return NULL;
            }
            if (strcmp(name, "slab_size_per_shard") == 0) {
                slab_size_per_shard = val_ull;
            } else {
                initial_value_capacity = val_ull;
            }
        } else if (strcmp(name, "gil_threshold") == 0) {
            long long val_ll = PyLong_AsLongLong(val);
            if (val_ll == -1 && PyErr_Occurred()) {
                return NULL;
            }
            if (val_ll < 0) {
                PyErr_SetString(PyExc_ValueError, "gil_threshold must be a non-negative integer");
                return NULL;
            }
            gil_threshold = val_ll;
        } else {
            PyErr_Format(PyExc_TypeError, "Cache() got an unexpected keyword argument '%s'", name);
            return NULL;
        }
    }

    if (slab_size_per_shard > (unsigned long long)SIZE_MAX) {
        PyErr_SetString(PyExc_OverflowError, "slab size does not fit size_t");
        return NULL;
    }
    if (initial_value_capacity > (unsigned long long)PY_SSIZE_T_MAX) {
        PyErr_SetString(
            PyExc_OverflowError,
            "initial value capacity does not fit a Python bytes object");
        return NULL;
    }

    CaeneusCache *self = (CaeneusCache *)CaeneusCacheType.tp_alloc(&CaeneusCacheType, 0);
    if (self == NULL) {
        return NULL;
    }
    self->handle = NULL;
    atomic_init(&self->active_calls, 0);
    self->gil_probe = 1024;
    self->concurrent_mode = 0;
    self->get_capacity = (Py_ssize_t)initial_value_capacity;
    self->gil_threshold = (Py_ssize_t)gil_threshold;
    for (int s = 0; s < FREELIST_SLOTS; s++) {
        for (int d = 0; d < FREELIST_DEPTH; d++) {
            self->get_freelist[s][d] = NULL;
            self->freelist_capacity[s][d] = 0;
        }
    }


    void *handle = NULL;
    Py_BEGIN_ALLOW_THREADS
    handle = caeneus_init(
        num_shards,
        slots_per_shard,
        (size_t)slab_size_per_shard);
    Py_END_ALLOW_THREADS

    if (handle == NULL) {
        Py_DECREF(self);
        PyErr_SetString(
            PyExc_RuntimeError,
            "caeneus initialization failed; check shard, slot, and slab settings");
        return NULL;
    }

    self->handle = handle;
    return (PyObject *)self;
}

static PyMethodDef module_methods[] = {
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef caeneus_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "caeneus",
    .m_doc =
        "Package caeneus provides a client for the Caeneus in-memory cache.",
    .m_size = -1,
    .m_methods = module_methods,
};

PyMODINIT_FUNC
PyInit_caeneus(void) {
    CaeneusCacheType.tp_vectorcall = (vectorcallfunc)cache_vectorcall;
    CaeneusCacheType.tp_flags |= Py_TPFLAGS_HAVE_VECTORCALL;

    if (PyType_Ready(&CaeneusCacheType) < 0) {
        return NULL;
    }

    PyObject *module = PyModule_Create(&caeneus_module);
    if (module == NULL) {
        return NULL;
    }

    Py_INCREF(&CaeneusCacheType);
    if (PyModule_AddObject(module, "Cache", (PyObject *)&CaeneusCacheType) < 0) {
        Py_DECREF(&CaeneusCacheType);
        Py_DECREF(module);
        return NULL;
    }
    return module;
}
