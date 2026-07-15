#define PY_SSIZE_T_CLEAN

#include <Python.h>

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdatomic.h>

#include "caeneus.h"

typedef struct {
    PyObject_HEAD
    void *handle;
    _Atomic unsigned int active_calls;
    unsigned int gil_probe;
    int concurrent_mode;
    /* GIL-protected initial capacity for one-copy small-value reads. */
    Py_ssize_t get_capacity;
    /* Retained bytes object for get() reuse when the caller dropped the prior result. */
    PyObject *get_freelist;
    Py_ssize_t freelist_capacity;
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
static inline int
get_immutable_bytes(PyObject *object,
                    const unsigned char **data,
                    Py_ssize_t *length) {
    if (PyUnicode_Check(object)) {
        Py_ssize_t unicode_length = 0;
        const char *utf8 = PyUnicode_AsUTF8AndSize(object, &unicode_length);
        if (utf8 == NULL) {
            return -1;
        }
        *data = (const unsigned char *)utf8;
        *length = unicode_length;
        return 0;
    }

    if (PyBytes_Check(object)) {
        *data = (const unsigned char *)PyBytes_AS_STRING(object);
        *length = PyBytes_GET_SIZE(object);
        return 0;
    }

    PyErr_SetString(
        PyExc_TypeError,
        "key and value must be str or bytes");
    return -1;
}

static int
get_open_handle(CaeneusCache *self, void **handle) {
    *handle = self->handle;
    if (*handle != NULL) {
        return 0;
    }
    PyErr_SetString(PyExc_RuntimeError, "caeneus cache is closed");
    return -1;
}

static int
begin_native_call(CaeneusCache *self, void **handle) {
    if (get_open_handle(self, handle) < 0) {
        return -1;
    }
    atomic_fetch_add_explicit(&self->active_calls, 1, memory_order_acquire);
    return 0;
}

static void
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

static int
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

static int
native_set(CaeneusCache *self,
           const unsigned char *key_data,
           Py_ssize_t key_length,
           const unsigned char *value_data,
           Py_ssize_t value_length,
           int *status) {
    const int release_gil = should_release_gil(self);
    if (!release_gil) {
        void *handle = NULL;
        if (get_open_handle(self, &handle) < 0) {
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
    Py_XDECREF(self->get_freelist);
    self->get_freelist = NULL;
    self->freelist_capacity = 0;
}

/*
 * acquire_get_output returns a writable bytes object of at least `capacity`
 * bytes. When the freelist object is exclusively owned and large enough it is
 * reused; otherwise a fresh bytes object is allocated.
 */
static PyObject *
acquire_get_output(CaeneusCache *self, Py_ssize_t capacity) {
    PyObject *output = self->get_freelist;
    if (output != NULL &&
        Py_REFCNT(output) == 1 &&
        self->freelist_capacity >= capacity) {
        self->get_freelist = NULL;
        Py_SET_SIZE((PyVarObject *)output, self->freelist_capacity);
        ((char *)PyBytes_AS_STRING(output))[self->freelist_capacity] = '\0';
        return output;
    }
    clear_get_freelist(self);
    return PyBytes_FromStringAndSize(NULL, capacity);
}

static void
store_get_freelist(CaeneusCache *self, PyObject *output, Py_ssize_t capacity) {
    if (self->get_freelist == NULL) {
        self->get_freelist = output;
        Py_INCREF(output);
        self->freelist_capacity = capacity;
    }
}

static void
abandon_get_output(CaeneusCache *self, PyObject *output, Py_ssize_t capacity) {
    if (output == NULL) {
        return;
    }
    if (self->get_freelist == NULL && Py_REFCNT(output) == 1) {
        self->get_freelist = output;
        self->freelist_capacity = capacity;
        return;
    }
    Py_DECREF(output);
}

static int
native_get(CaeneusCache *self,
           const unsigned char *key_data,
           Py_ssize_t key_length,
           unsigned char *buffer,
           Py_ssize_t buffer_length,
           uint64_t *packed) {
    /*
     * Tiny single-thread reads stay on the GIL (handoff costs more than the
     * lookup). Once concurrent_mode latches, release even for small buffers so
     * shared multi-worker workloads can overlap.
     */
    if (!self->concurrent_mode &&
        atomic_load_explicit(&self->active_calls, memory_order_relaxed) != 0) {
        self->concurrent_mode = 1;
    }
    const int release_gil =
        self->concurrent_mode
            ? 1
            : (buffer_length > 1024 && should_release_gil(self));
    if (!release_gil) {
        void *handle = NULL;
        if (get_open_handle(self, &handle) < 0) {
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
        NULL,
    };
    unsigned int num_shards = 64;
    unsigned int slots_per_shard = 1024;
    unsigned long long slab_size_per_shard = 1024ULL * 1024ULL;
    unsigned long long initial_value_capacity = 128;

    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "|IIKK:Cache",
            keywords,
            &num_shards,
            &slots_per_shard,
            &slab_size_per_shard,
            &initial_value_capacity)) {
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

    if (self->handle != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "caeneus cache is already initialized");
        return -1;
    }

    atomic_init(&self->active_calls, 0);
    self->gil_probe = 1024;
    self->concurrent_mode = 0;
    self->get_freelist = NULL;
    self->freelist_capacity = 0;

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

static int
cache_set_impl(CaeneusCache *self, PyObject *key_object, PyObject *value_object) {
    const unsigned char *key_data = NULL;
    const unsigned char *value_data = NULL;
    Py_ssize_t key_length = 0;
    Py_ssize_t value_length = 0;
    void *handle = NULL;

    if (get_open_handle(self, &handle) < 0 ||
        get_immutable_bytes(key_object, &key_data, &key_length) < 0 ||
        get_immutable_bytes(value_object, &value_data, &value_length) < 0) {
        return -1;
    }

    int status = CAENEUS_ERR_PANIC;
    if (native_set(
            self,
            key_data,
            key_length,
            value_data,
            value_length,
            &status) < 0) {
        return -1;
    }

    if (status != CAENEUS_OK) {
        PyErr_SetString(PyExc_RuntimeError, "caeneus set failed");
        return -1;
    }
    return 0;
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

static PyObject *
cache_get_impl(CaeneusCache *self, PyObject *key_object, int is_subscript) {
    const unsigned char *key_data = NULL;
    Py_ssize_t key_length = 0;
    void *open_handle = NULL;

    if (get_open_handle(self, &open_handle) < 0 ||
        get_immutable_bytes(key_object, &key_data, &key_length) < 0) {
        return NULL;
    }

    unsigned char stack_buf[1024];
    PyObject *output = NULL;
    Py_ssize_t output_capacity = self->get_capacity;
    Py_ssize_t owned_capacity = 0;
    uint64_t packed = 0;
    if (output_capacity > 0) {
        output = acquire_get_output(self, output_capacity);
        if (output == NULL) {
            return NULL;
        }
        owned_capacity = PyBytes_GET_SIZE(output);
        if (native_get(
                self,
                key_data,
                key_length,
                (unsigned char *)PyBytes_AS_STRING(output),
                owned_capacity,
                &packed) < 0) {
            abandon_get_output(self, output, owned_capacity);
            return NULL;
        }
        output_capacity = owned_capacity;
    } else {
        if (native_get(
                self,
                key_data,
                key_length,
                stack_buf,
                sizeof(stack_buf),
                &packed) < 0) {
            return NULL;
        }
    }

    int32_t status = result_code(packed);
    uint32_t required_length = result_length(packed);

    if (status == CAENEUS_MISS) {
        abandon_get_output(self, output, output_capacity);
        if (is_subscript) {
            PyErr_SetObject(PyExc_KeyError, key_object);
            return NULL;
        }
        Py_RETURN_NONE;
    }
    if (status == CAENEUS_ERR_PANIC) {
        abandon_get_output(self, output, output_capacity);
        PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
        return NULL;
    }
    if (status == CAENEUS_OK) {
        if (output != NULL) {
            if ((uint64_t)required_length > (uint64_t)output_capacity) {
                abandon_get_output(self, output, output_capacity);
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
            self->get_capacity = (Py_ssize_t)required_length;
            store_get_freelist(self, output, output_capacity);
            return output;
        }
        return PyBytes_FromStringAndSize(
            (const char *)stack_buf,
            (Py_ssize_t)required_length);
    }

    // Fallback: value is larger than the current buffer.
    if (status != CAENEUS_ERR_SMALL_BUF) {
        abandon_get_output(self, output, output_capacity);
        PyErr_SetString(PyExc_RuntimeError, "caeneus returned an invalid get status");
        return NULL;
    }

    // Retry loop for the fallback heap path
    abandon_get_output(self, output, output_capacity);
    output = NULL;
    output_capacity = (Py_ssize_t)required_length;
    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        if (output_capacity == 0) {
            // Re-probe with zero-byte buffer if capacity became 0 due to eviction
            unsigned char probe = 0;
            if (native_get(
                    self,
                    key_data,
                    key_length,
                    &probe,
                    0,
                    &packed) < 0) {
                return NULL;
            }
            status = result_code(packed);
            required_length = result_length(packed);
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
            // If it returns CAENEUS_OK for zero-byte probe:
            if (required_length == 0) {
                self->get_capacity = 0;
                return PyBytes_FromStringAndSize("", 0);
            }
            PyErr_SetString(PyExc_RuntimeError, "caeneus returned success for a zero-byte probe");
            return NULL;
        }

        if ((uint64_t)output_capacity > (uint64_t)PY_SSIZE_T_MAX) {
            PyErr_SetString(PyExc_OverflowError, "caeneus value is too large for a Python bytes object");
            return NULL;
        }

        output = acquire_get_output(self, output_capacity);
        if (output == NULL) {
            return NULL;
        }
        owned_capacity = PyBytes_GET_SIZE(output);

        if (native_get(
                self,
                key_data,
                key_length,
                (unsigned char *)PyBytes_AS_STRING(output),
                owned_capacity,
                &packed) < 0) {
            abandon_get_output(self, output, owned_capacity);
            return NULL;
        }

        status = result_code(packed);
        required_length = result_length(packed);

        if (status == CAENEUS_MISS) {
            abandon_get_output(self, output, owned_capacity);
            if (is_subscript) {
                PyErr_SetObject(PyExc_KeyError, key_object);
                return NULL;
            }
            Py_RETURN_NONE;
        }
        if (status == CAENEUS_ERR_PANIC) {
            abandon_get_output(self, output, owned_capacity);
            PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
            return NULL;
        }
        if (status == CAENEUS_OK) {
            if ((uint64_t)required_length > (uint64_t)owned_capacity) {
                abandon_get_output(self, output, owned_capacity);
                PyErr_SetString(PyExc_RuntimeError, "caeneus returned a value larger than its output buffer");
                return NULL;
            }
            if ((Py_ssize_t)required_length < owned_capacity) {
                unsigned char *output_data = (unsigned char *)PyBytes_AS_STRING(output);
                output_data[required_length] = '\0';
                Py_SET_SIZE((PyVarObject *)output, (Py_ssize_t)required_length);
            }
            self->get_capacity = (Py_ssize_t)required_length;
            store_get_freelist(self, output, owned_capacity);
            return output;
        }

        // If status == CAENEUS_ERR_SMALL_BUF
        abandon_get_output(self, output, owned_capacity);
        output = NULL;
        output_capacity = (Py_ssize_t)required_length;
    }

    PyErr_SetString(
        PyExc_RuntimeError,
        "caeneus get could not stabilize during concurrent updates");
    return NULL;
}

static PyObject *
cache_get(CaeneusCache *self, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs != 1) {
        PyErr_Format(
            PyExc_TypeError,
            "get() takes exactly 1 argument (%zd given)",
            nargs);
        return NULL;
    }
    return cache_get_impl(self, args[0], 0);
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
    PyObject *key_object = NULL;
    const unsigned char *key_data = NULL;
    Py_ssize_t key_length = 0;
    Py_buffer output_view;
    int is_generic_buffer = 0;
    unsigned char *buf_ptr = NULL;
    Py_ssize_t buf_len = 0;
    void *open_handle = NULL;

    if (nargs != 2) {
        PyErr_Format(
            PyExc_TypeError,
            "get_into() takes exactly 2 arguments (%zd given)",
            nargs);
        return NULL;
    }
    key_object = args[0];
    if (get_open_handle(self, &open_handle) < 0 ||
        get_immutable_bytes(key_object, &key_data, &key_length) < 0) {
        return NULL;
    }

    PyObject *target = args[1];
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
                key_data,
                key_length,
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
        (PyCFunction)(void(*)(void))cache_get,
        METH_FASTCALL,
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
        } else {
            PyErr_SetString(PyExc_TypeError, "Cache() takes at most 4 positional arguments");
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
    self->get_freelist = NULL;
    self->freelist_capacity = 0;

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
