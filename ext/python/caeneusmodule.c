#define PY_SSIZE_T_CLEAN

#include <Python.h>

#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#include "caeneus.h"

typedef struct {
    PyObject_HEAD
    void *handle;
    /* get_capacity is a GIL-protected hint used to skip repeated probe calls. */
    Py_ssize_t get_capacity;
} CaeneusCache;

static int32_t result_code(uint64_t packed) {
    return (int32_t)(uint32_t)(packed >> 32);
}

static uint32_t result_length(uint64_t packed) {
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
static int
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
    unsigned long long initial_value_capacity = 0;

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
    if (self->handle != NULL) {
        void *handle = self->handle;
        self->handle = NULL;
        self->get_capacity = 0;
        Py_BEGIN_ALLOW_THREADS
        caeneus_deinit(handle);
        Py_END_ALLOW_THREADS
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
cache_close(CaeneusCache *self, PyObject *Py_UNUSED(ignored)) {
    if (self->handle != NULL) {
        void *handle = self->handle;
        self->handle = NULL;
        self->get_capacity = 0;
        Py_BEGIN_ALLOW_THREADS
        caeneus_deinit(handle);
        Py_END_ALLOW_THREADS
    }
    Py_RETURN_NONE;
}

static PyObject *
cache_set(CaeneusCache *self, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *key_object = NULL;
    PyObject *value_object = NULL;
    const unsigned char *key_data = NULL;
    const unsigned char *value_data = NULL;
    Py_ssize_t key_length = 0;
    Py_ssize_t value_length = 0;
    void *handle = NULL;

    if (nargs != 2) {
        PyErr_Format(
            PyExc_TypeError,
            "set() takes exactly 2 arguments (%zd given)",
            nargs);
        return NULL;
    }
    key_object = args[0];
    value_object = args[1];
    if (get_open_handle(self, &handle) < 0 ||
        get_immutable_bytes(key_object, &key_data, &key_length) < 0 ||
        get_immutable_bytes(value_object, &value_data, &value_length) < 0) {
        return NULL;
    }

    int status = caeneus_set(
        handle,
        key_data,
        (size_t)key_length,
        value_data,
        (size_t)value_length);

    if (status != CAENEUS_OK) {
        PyErr_SetString(PyExc_RuntimeError, "caeneus set failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
cache_get(CaeneusCache *self, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *key_object = NULL;
    const unsigned char *key_data = NULL;
    Py_ssize_t key_length = 0;

    if (nargs != 1) {
        PyErr_Format(
            PyExc_TypeError,
            "get() takes exactly 1 argument (%zd given)",
            nargs);
        return NULL;
    }
    key_object = args[0];

    void *handle = NULL;
    if (get_open_handle(self, &handle) < 0 ||
        get_immutable_bytes(key_object, &key_data, &key_length) < 0) {
        return NULL;
    }

    // Try stack allocation path first for small values
    unsigned char stack_buf[1024];
    uint64_t packed = caeneus_get(
        handle,
        key_data,
        (size_t)key_length,
        stack_buf,
        sizeof(stack_buf));

    int32_t status = result_code(packed);
    uint32_t required_length = result_length(packed);

    if (status == CAENEUS_MISS) {
        Py_RETURN_NONE;
    }
    if (status == CAENEUS_ERR_PANIC) {
        PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
        return NULL;
    }
    if (status == CAENEUS_OK) {
        if ((size_t)required_length > sizeof(stack_buf)) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "caeneus returned a value larger than stack buffer");
            return NULL;
        }
        return PyBytes_FromStringAndSize((const char *)stack_buf, (Py_ssize_t)required_length);
    }

    // Fallback: value is larger than 1024 bytes.
    if (status != CAENEUS_ERR_SMALL_BUF) {
        PyErr_SetString(PyExc_RuntimeError, "caeneus returned an invalid get status");
        return NULL;
    }

    // Retry loop for the fallback heap path
    Py_ssize_t output_capacity = (Py_ssize_t)required_length;
    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        if (output_capacity == 0) {
            // Re-probe with zero-byte buffer if capacity became 0 due to eviction
            unsigned char probe = 0;
            packed = caeneus_get(
                handle,
                key_data,
                (size_t)key_length,
                &probe,
                0);
            status = result_code(packed);
            required_length = result_length(packed);
            if (status == CAENEUS_MISS) {
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
                return PyBytes_FromStringAndSize("", 0);
            }
            PyErr_SetString(PyExc_RuntimeError, "caeneus returned success for a zero-byte probe");
            return NULL;
        }

        if ((uint64_t)output_capacity > (uint64_t)PY_SSIZE_T_MAX) {
            PyErr_SetString(PyExc_OverflowError, "caeneus value is too large for a Python bytes object");
            return NULL;
        }

        PyObject *output = PyBytes_FromStringAndSize(NULL, output_capacity);
        if (output == NULL) {
            return NULL;
        }

        packed = caeneus_get(
            handle,
            key_data,
            (size_t)key_length,
            (unsigned char *)PyBytes_AS_STRING(output),
            (size_t)output_capacity);

        status = result_code(packed);
        required_length = result_length(packed);

        if (status == CAENEUS_MISS) {
            Py_DECREF(output);
            Py_RETURN_NONE;
        }
        if (status == CAENEUS_ERR_PANIC) {
            Py_DECREF(output);
            PyErr_SetString(PyExc_RuntimeError, "caeneus get failed");
            return NULL;
        }
        if (status == CAENEUS_OK) {
            if ((uint64_t)required_length > (uint64_t)output_capacity) {
                Py_DECREF(output);
                PyErr_SetString(PyExc_RuntimeError, "caeneus returned a value larger than its output buffer");
                return NULL;
            }
            // If the returned value is smaller than allocated capacity, we need to resize
            if ((Py_ssize_t)required_length < output_capacity) {
                unsigned char *output_data = (unsigned char *)PyBytes_AS_STRING(output);
                output_data[required_length] = '\0';
                Py_SET_SIZE((PyVarObject *)output, (Py_ssize_t)required_length);
            }
            return output;
        }

        // If status == CAENEUS_ERR_SMALL_BUF
        Py_DECREF(output);
        output_capacity = (Py_ssize_t)required_length;
    }

    PyErr_SetString(
        PyExc_RuntimeError,
        "caeneus get could not stabilize during concurrent updates");
    return NULL;
}

static PyObject *
cache_get_into(CaeneusCache *self, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *key_object = NULL;
    const unsigned char *key_data = NULL;
    Py_ssize_t key_length = 0;
    Py_buffer output_view;
    void *handle = NULL;

    if (nargs != 2) {
        PyErr_Format(
            PyExc_TypeError,
            "get_into() takes exactly 2 arguments (%zd given)",
            nargs);
        return NULL;
    }
    key_object = args[0];
    if (get_open_handle(self, &handle) < 0 ||
        get_immutable_bytes(key_object, &key_data, &key_length) < 0) {
        return NULL;
    }
    if (PyObject_GetBuffer(
            args[1],
            &output_view,
            PyBUF_WRITABLE | PyBUF_C_CONTIGUOUS) < 0) {
        return NULL;
    }

    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        uint64_t packed = caeneus_get(
            handle,
            key_data,
            (size_t)key_length,
            (unsigned char *)output_view.buf,
            (size_t)output_view.len);

        int32_t status = result_code(packed);
        uint32_t output_length = result_length(packed);
        if (status == CAENEUS_MISS) {
            PyBuffer_Release(&output_view);
            Py_RETURN_NONE;
        }
        if (status == CAENEUS_OK) {
            if ((uint64_t)output_length > (uint64_t)output_view.len) {
                PyBuffer_Release(&output_view);
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "caeneus returned a value larger than its output buffer");
                return NULL;
            }
            PyBuffer_Release(&output_view);
            return PyLong_FromUnsignedLong((unsigned long)output_length);
        }
        if (status == CAENEUS_ERR_SMALL_BUF) {
            if (output_length == 0) {
                continue;
            }
            PyBuffer_Release(&output_view);
            PyErr_Format(
                PyExc_BufferError,
                "caeneus value requires %u bytes",
                output_length);
            return NULL;
        }

        PyBuffer_Release(&output_view);
        PyErr_SetString(PyExc_RuntimeError, "caeneus get_into failed");
        return NULL;
    }

    PyBuffer_Release(&output_view);
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
        "initial_value_capacity optionally seeds the read buffer size; zero "
        "uses a size probe on the first read.",
    .tp_basicsize = sizeof(CaeneusCache),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = cache_methods,
    .tp_init = (initproc)cache_init,
    .tp_new = PyType_GenericNew,
    .tp_dealloc = (destructor)cache_dealloc,
};

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
