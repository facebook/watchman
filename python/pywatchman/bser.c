/*
Copyright (c) 2013-2015, Facebook, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific
   prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <Python.h>
#ifdef _MSC_VER
#define inline __inline
#include <stdint.h>
#endif

/* Return the smallest size int that can store the value */
#define INT_SIZE(x) (((x) == ((int8_t)x))  ? 1 :    \
                     ((x) == ((int16_t)x)) ? 2 :    \
                     ((x) == ((int32_t)x)) ? 4 : 8)

#define BSER_ARRAY     0x00
#define BSER_OBJECT    0x01
#define BSER_STRING    0x02
#define BSER_INT8      0x03
#define BSER_INT16     0x04
#define BSER_INT32     0x05
#define BSER_INT64     0x06
#define BSER_REAL      0x07
#define BSER_TRUE      0x08
#define BSER_FALSE     0x09
#define BSER_NULL      0x0a
#define BSER_TEMPLATE  0x0b
#define BSER_SKIP      0x0c

// An immutable object representation of BSER_OBJECT.
// Rather than build a hash table, key -> value are obtained
// by walking the list of keys to determine the offset into
// the values array.  The assumption is that the number of
// array elements will be typically small (~6 for the top
// level query result and typically 3 for the file entries)
// so that the time overhead for this is small compared to
// using a proper hash table.
typedef struct {
  PyObject_HEAD
  PyObject *keys;   // tuple of field names
  PyObject *values; // tuple of values
} bserObject;

static Py_ssize_t bserobj_tuple_length(PyObject *o) {
  bserObject *obj = (bserObject*)o;

  return PySequence_Length(obj->keys);
}

static PyObject *bserobj_tuple_item(PyObject *o, Py_ssize_t i) {
  bserObject *obj = (bserObject*)o;

  return PySequence_GetItem(obj->values, i);
}

static PySequenceMethods bserobj_sq = {
  bserobj_tuple_length,      /* sq_length */
  0,                         /* sq_concat */
  0,                         /* sq_repeat */
  bserobj_tuple_item,        /* sq_item */
  0,                         /* sq_ass_item */
  0,                         /* sq_contains */
  0,                         /* sq_inplace_concat */
  0                          /* sq_inplace_repeat */
};

static void bserobj_dealloc(PyObject *o) {
  bserObject *obj = (bserObject*)o;

  Py_CLEAR(obj->keys);
  Py_CLEAR(obj->values);
  PyObject_Del(o);
}

static PyObject *bserobj_getattrro(PyObject *o, PyObject *name) {
  bserObject *obj = (bserObject*)o;
  Py_ssize_t i, n;
  const char *namestr;

  if (PyIndex_Check(name)) {
    i = PyNumber_AsSsize_t(name, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred()) {
      return NULL;
    }
    return PySequence_GetItem(obj->values, i);
  }

  // hack^Wfeature to allow mercurial to use "st_size" to reference "size"
  namestr = PyString_AsString(name);
  if (!strncmp(namestr, "st_", 3)) {
    namestr += 3;
  }

  n = PyTuple_GET_SIZE(obj->keys);
  for (i = 0; i < n; i++) {
    const char *item_name = NULL;

    PyObject *key = PyTuple_GetItem(obj->keys, i);
    if (!key) {
      return NULL;
    }

    item_name = PyString_AsString(key);
    if (!strcmp(item_name, namestr)) {
      return PySequence_GetItem(obj->values, i);
    }
  }
  PyErr_Format(PyExc_AttributeError,
              "bserobject has no attribute '%.400s'", namestr);
  return NULL;
}

static PyMappingMethods bserobj_map = {
  bserobj_tuple_length,     /* mp_length */
  bserobj_getattrro,        /* mp_subscript */
  0                         /* mp_ass_subscript */
};

PyTypeObject bserObjectType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  "bserobj_tuple",           /* tp_name */
  sizeof(bserObject),        /* tp_basicsize */
  0,                         /* tp_itemsize */
  bserobj_dealloc,           /* tp_dealloc */
  0,                         /* tp_print */
  0,                         /* tp_getattr */
  0,                         /* tp_setattr */
  0,                         /* tp_compare */
  0,                         /* tp_repr */
  0,                         /* tp_as_number */
  &bserobj_sq,               /* tp_as_sequence */
  &bserobj_map,              /* tp_as_mapping */
  0,                         /* tp_hash  */
  0,                         /* tp_call */
  0,                         /* tp_str */
  bserobj_getattrro,         /* tp_getattro */
  0,                         /* tp_setattro */
  0,                         /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT,        /* tp_flags */
  "bserobj tuple",           /* tp_doc */
  0,                         /* tp_traverse */
  0,                         /* tp_clear */
  0,                         /* tp_richcompare */
  0,                         /* tp_weaklistoffset */
  0,                         /* tp_iter */
  0,                         /* tp_iternext */
  0,                         /* tp_methods */
  0,                         /* tp_members */
  0,                         /* tp_getset */
  0,                         /* tp_base */
  0,                         /* tp_dict */
  0,                         /* tp_descr_get */
  0,                         /* tp_descr_set */
  0,                         /* tp_dictoffset */
  0,                         /* tp_init */
  0,                         /* tp_alloc */
  0,                         /* tp_new */
};


static PyObject *bser_loads_recursive(const char **ptr, const char *end,
    int mutable);

static const char bser_true = BSER_TRUE;
static const char bser_false = BSER_FALSE;
static const char bser_null = BSER_NULL;
static const char bser_string_hdr = BSER_STRING;
static const char bser_array_hdr = BSER_ARRAY;
static const char bser_object_hdr = BSER_OBJECT;

static inline uint32_t next_power_2(uint32_t n)
{
  n |= (n >> 16);
  n |= (n >> 8);
  n |= (n >> 4);
  n |= (n >> 2);
  n |= (n >> 1);
  return n + 1;
}

// A buffer we use for building up the serialized result
struct bser_buffer {
  char *buf;
  int wpos, allocd;
};
typedef struct bser_buffer bser_t;

static int bser_append(bser_t *bser, const char *data, uint32_t len)
{
  int newlen = next_power_2(bser->wpos + len);
  if (newlen > bser->allocd) {
    char *nbuf = realloc(bser->buf, newlen);
    if (!nbuf) {
      return 0;
    }

    bser->buf = nbuf;
    bser->allocd = newlen;
  }

  memcpy(bser->buf + bser->wpos, data, len);
  bser->wpos += len;
  return 1;
}

static int bser_init(bser_t *bser)
{
  bser->allocd = 8192;
  bser->wpos = 0;
  bser->buf = malloc(bser->allocd);

  if (!bser->buf) {
    return 0;
  }

  // Leave room for the serialization header, which includes
  // our overall length.  To make things simpler, we'll use an
  // int32 for the header
#define EMPTY_HEADER "\x00\x01\x05\x00\x00\x00\x00"
  bser_append(bser, EMPTY_HEADER, sizeof(EMPTY_HEADER)-1);

  return 1;
}

static void bser_dtor(bser_t *bser)
{
  free(bser->buf);
  bser->buf = NULL;
}

static int bser_long(bser_t *bser, int64_t val)
{
  int8_t i8;
  int16_t i16;
  int32_t i32;
  int64_t i64;
  char sz;
  int size = INT_SIZE(val);
  char *iptr;

  switch (size) {
    case 1:
      sz = BSER_INT8;
      i8 = (int8_t)val;
      iptr = (char*)&i8;
      break;
    case 2:
      sz = BSER_INT16;
      i16 = (int16_t)val;
      iptr = (char*)&i16;
      break;
    case 4:
      sz = BSER_INT32;
      i32 = (int32_t)val;
      iptr = (char*)&i32;
      break;
    case 8:
      sz = BSER_INT64;
      i64 = (int64_t)val;
      iptr = (char*)&i64;
      break;
    default:
      PyErr_SetString(PyExc_RuntimeError,
          "Cannot represent this long value!?");
      return 0;
  }

  if (!bser_append(bser, &sz, sizeof(sz))) {
    return 0;
  }

  return bser_append(bser, iptr, size);
}

static int bser_string(bser_t *bser, PyObject *sval)
{
  char *buf = NULL;
  Py_ssize_t len;
  int res;
  PyObject *utf = NULL;

  if (PyUnicode_Check(sval)) {
    utf = PyUnicode_AsEncodedString(sval, "utf-8", "ignore");
    sval = utf;
  }

  res = PyString_AsStringAndSize(sval, &buf, &len);
  if (res == -1) {
    res = 0;
    goto out;
  }

  if (!bser_append(bser, &bser_string_hdr, sizeof(bser_string_hdr))) {
    res = 0;
    goto out;
  }

  if (!bser_long(bser, len)) {
    res = 0;
    goto out;
  }

  if (len > UINT32_MAX) {
    PyErr_Format(PyExc_ValueError, "string too big");
    res = 0;
    goto out;
  }

  res = bser_append(bser, buf, (uint32_t)len);

out:
  if (utf) {
    Py_DECREF(utf);
  }

  return res;
}

static int bser_recursive(bser_t *bser, PyObject *val)
{
  if (PyBool_Check(val)) {
    if (val == Py_True) {
      return bser_append(bser, &bser_true, sizeof(bser_true));
    }
    return bser_append(bser, &bser_false, sizeof(bser_false));
  }

  if (val == Py_None) {
    return bser_append(bser, &bser_null, sizeof(bser_null));
  }

  if (PyInt_Check(val)) {
    return bser_long(bser, PyInt_AS_LONG(val));
  }

  if (PyLong_Check(val)) {
    return bser_long(bser, PyLong_AsLongLong(val));
  }

  if (PyString_Check(val) || PyUnicode_Check(val)) {
    return bser_string(bser, val);
  }


  if (PyFloat_Check(val)) {
    double dval = PyFloat_AS_DOUBLE(val);
    char sz = BSER_REAL;

    if (!bser_append(bser, &sz, sizeof(sz))) {
      return 0;
    }

    return bser_append(bser, (char*)&dval, sizeof(dval));
  }

  if (PyList_Check(val)) {
    Py_ssize_t i, len = PyList_GET_SIZE(val);

    if (!bser_append(bser, &bser_array_hdr, sizeof(bser_array_hdr))) {
      return 0;
    }

    if (!bser_long(bser, len)) {
      return 0;
    }

    for (i = 0; i < len; i++) {
      PyObject *ele = PyList_GET_ITEM(val, i);

      if (!bser_recursive(bser, ele)) {
        return 0;
      }
    }

    return 1;
  }

  if (PyTuple_Check(val)) {
    Py_ssize_t i, len = PyTuple_GET_SIZE(val);

    if (!bser_append(bser, &bser_array_hdr, sizeof(bser_array_hdr))) {
      return 0;
    }

    if (!bser_long(bser, len)) {
      return 0;
    }

    for (i = 0; i < len; i++) {
      PyObject *ele = PyTuple_GET_ITEM(val, i);

      if (!bser_recursive(bser, ele)) {
        return 0;
      }
    }

    return 1;
  }

  if (PyMapping_Check(val)) {
    Py_ssize_t len = PyMapping_Length(val);
    Py_ssize_t pos = 0;
    PyObject *key, *ele;

    if (!bser_append(bser, &bser_object_hdr, sizeof(bser_object_hdr))) {
      return 0;
    }

    if (!bser_long(bser, len)) {
      return 0;
    }

    while (PyDict_Next(val, &pos, &key, &ele)) {
      if (!bser_string(bser, key)) {
        return 0;
      }
      if (!bser_recursive(bser, ele)) {
        return 0;
      }
    }

    return 1;
  }

  PyErr_SetString(PyExc_ValueError, "Unsupported value type");
  return 0;
}

static PyObject *bser_dumps(PyObject *self, PyObject *args)
{
  PyObject *val = NULL, *res;
  bser_t bser;
  uint32_t len;

  if (!PyArg_ParseTuple(args, "O", &val)) {
    return NULL;
  }

  if (!bser_init(&bser)) {
    return PyErr_NoMemory();
  }

  if (!bser_recursive(&bser, val)) {
    bser_dtor(&bser);
    if (errno == ENOMEM) {
      return PyErr_NoMemory();
    }
    // otherwise, we've already set the error to something reasonable
    return NULL;
  }

  // Now fill in the overall length
  len = bser.wpos - (sizeof(EMPTY_HEADER) - 1);
  memcpy(bser.buf + 3, &len, sizeof(len));

  res = PyString_FromStringAndSize(bser.buf, bser.wpos);
  bser_dtor(&bser);

  return res;
}

int bunser_int(const char **ptr, const char *end, int64_t *val)
{
  int needed;
  const char *buf = *ptr;
  int8_t i8;
  int16_t i16;
  int32_t i32;
  int64_t i64;

  switch (buf[0]) {
    case BSER_INT8:
      needed = 2;
      break;
    case BSER_INT16:
      needed = 3;
      break;
    case BSER_INT32:
      needed = 5;
      break;
    case BSER_INT64:
      needed = 9;
      break;
    default:
      PyErr_Format(PyExc_ValueError,
          "invalid bser int encoding 0x%02x", buf[0]);
      return 0;
  }
  if (end - buf < needed) {
    PyErr_SetString(PyExc_ValueError, "input buffer to small for int encoding");
    return 0;
  }
  *ptr = buf + needed;
  switch (buf[0]) {
    case BSER_INT8:
      memcpy(&i8, buf + 1, sizeof(i8));
      *val = i8;
      return 1;
    case BSER_INT16:
      memcpy(&i16, buf + 1, sizeof(i16));
      *val = i16;
      return 1;
    case BSER_INT32:
      memcpy(&i32, buf + 1, sizeof(i32));
      *val = i32;
      return 1;
    case BSER_INT64:
      memcpy(&i64, buf + 1, sizeof(i64));
      *val = i64;
      return 1;
    default:
      return 0;
  }
}

static int bunser_string(const char **ptr, const char *end,
    const char **start, int64_t *len)
{
  const char *buf = *ptr;

  // skip string marker
  buf++;
  if (!bunser_int(&buf, end, len)) {
    return 0;
  }

  if (buf + *len > end) {
    PyErr_Format(PyExc_ValueError, "invalid string length in bser data");
    return 0;
  }

  *ptr = buf + *len;
  *start = buf;
  return 1;
}

static PyObject *bunser_array(const char **ptr, const char *end, int mutable)
{
  const char *buf = *ptr;
  int64_t nitems, i;
  PyObject *res;

  // skip array header
  buf++;
  if (!bunser_int(&buf, end, &nitems)) {
    return 0;
  }
  *ptr = buf;

  if (nitems > LONG_MAX) {
    PyErr_Format(PyExc_ValueError, "too many items for python array");
    return NULL;
  }

  if (mutable) {
    res = PyList_New((Py_ssize_t)nitems);
  } else {
    res = PyTuple_New((Py_ssize_t)nitems);
  }

  for (i = 0; i < nitems; i++) {
    PyObject *ele = bser_loads_recursive(ptr, end, mutable);

    if (!ele) {
      Py_DECREF(res);
      return NULL;
    }

    if (mutable) {
      PyList_SET_ITEM(res, i, ele);
    } else {
      PyTuple_SET_ITEM(res, i, ele);
    }
    // DECREF(ele) not required as SET_ITEM steals the ref
  }

  return res;
}

static PyObject *bunser_object(const char **ptr, const char *end,
    int mutable)
{
  const char *buf = *ptr;
  int64_t nitems, i;
  PyObject *res;
  bserObject *obj;

  // skip array header
  buf++;
  if (!bunser_int(&buf, end, &nitems)) {
    return 0;
  }
  *ptr = buf;

  if (mutable) {
    res = PyDict_New();
  } else {
    obj = PyObject_New(bserObject, &bserObjectType);
    obj->keys = PyTuple_New((Py_ssize_t)nitems);
    obj->values = PyTuple_New((Py_ssize_t)nitems);
    res = (PyObject*)obj;
  }

  for (i = 0; i < nitems; i++) {
    const char *keystr;
    int64_t keylen;
    PyObject *key;
    PyObject *ele;

    if (!bunser_string(ptr, end, &keystr, &keylen)) {
      Py_DECREF(res);
      return NULL;
    }

    if (keylen > LONG_MAX) {
      PyErr_Format(PyExc_ValueError, "string too big for python");
      Py_DECREF(res);
      return NULL;
    }

    key = PyString_FromStringAndSize(keystr, (Py_ssize_t)keylen);
    if (!key) {
      Py_DECREF(res);
      return NULL;
    }

    ele = bser_loads_recursive(ptr, end, mutable);

    if (!ele) {
      Py_DECREF(key);
      Py_DECREF(res);
      return NULL;
    }

    if (mutable) {
      PyDict_SetItem(res, key, ele);
      Py_DECREF(key);
      Py_DECREF(ele);
    } else {
      /* PyTuple_SET_ITEM steals ele, key */
      PyTuple_SET_ITEM(obj->values, i, ele);
      PyTuple_SET_ITEM(obj->keys, i, key);
    }
  }

  return res;
}

static PyObject *bunser_template(const char **ptr, const char *end,
    int mutable)
{
  const char *buf = *ptr;
  int64_t nitems, i;
  PyObject *arrval;
  PyObject *keys;
  Py_ssize_t numkeys, keyidx;

  if (buf[1] != BSER_ARRAY) {
    PyErr_Format(PyExc_ValueError, "Expect ARRAY to follow TEMPLATE");
    return NULL;
  }

  // skip header
  buf++;
  *ptr = buf;

  // Load template keys
  keys = bunser_array(ptr, end, mutable);
  if (!keys) {
    return NULL;
  }

  numkeys = PySequence_Length(keys);

  // Load number of array elements
  if (!bunser_int(ptr, end, &nitems)) {
    Py_DECREF(keys);
    return 0;
  }

  if (nitems > LONG_MAX) {
    PyErr_Format(PyExc_ValueError, "Too many items for python");
    Py_DECREF(keys);
    return NULL;
  }

  arrval = PyList_New((Py_ssize_t)nitems);
  if (!arrval) {
    Py_DECREF(keys);
    return NULL;
  }

  for (i = 0; i < nitems; i++) {
    PyObject *dict = NULL;
    bserObject *obj = NULL;

    if (mutable) {
      dict = PyDict_New();
    } else {
      obj = PyObject_New(bserObject, &bserObjectType);
      if (obj) {
        obj->keys = keys;
        Py_INCREF(obj->keys);
        obj->values = PyTuple_New(numkeys);
      }
      dict = (PyObject*)obj;
    }
    if (!dict) {
fail:
      Py_DECREF(keys);
      Py_DECREF(arrval);
      return NULL;
    }

    for (keyidx = 0; keyidx < numkeys; keyidx++) {
      PyObject *key;
      PyObject *ele;

      if (**ptr == BSER_SKIP) {
        *ptr = *ptr + 1;
        ele = Py_None;
        Py_INCREF(ele);
      } else {
        ele = bser_loads_recursive(ptr, end, mutable);
      }

      if (!ele) {
        goto fail;
      }

      if (mutable) {
        key = PyList_GET_ITEM(keys, keyidx);
        PyDict_SetItem(dict, key, ele);
        Py_DECREF(ele);
      } else {
        PyTuple_SET_ITEM(obj->values, keyidx, ele);
        // DECREF(ele) not required as SET_ITEM steals the ref
      }
    }

    PyList_SET_ITEM(arrval, i, dict);
    // DECREF(obj) not required as SET_ITEM steals the ref
  }

  Py_DECREF(keys);

  return arrval;
}

static PyObject *bser_loads_recursive(const char **ptr, const char *end,
    int mutable)
{
  const char *buf = *ptr;

  switch (buf[0]) {
    case BSER_INT8:
    case BSER_INT16:
    case BSER_INT32:
    case BSER_INT64:
      {
        int64_t ival;
        if (!bunser_int(ptr, end, &ival)) {
          return NULL;
        }
        if (ival > LONG_MAX) {
          return PyLong_FromLongLong(ival);
        }
        return PyInt_FromLong((long)ival);
      }

    case BSER_REAL:
      {
        double dval;
        memcpy(&dval, buf + 1, sizeof(dval));
        *ptr = buf + 1 + sizeof(double);
        return PyFloat_FromDouble(dval);
      }

    case BSER_TRUE:
      *ptr = buf + 1;
      Py_INCREF(Py_True);
      return Py_True;

    case BSER_FALSE:
      *ptr = buf + 1;
      Py_INCREF(Py_False);
      return Py_False;

    case BSER_NULL:
      *ptr = buf + 1;
      Py_INCREF(Py_None);
      return Py_None;

    case BSER_STRING:
      {
        const char *start;
        int64_t len;

        if (!bunser_string(ptr, end, &start, &len)) {
          return NULL;
        }

        if (len > LONG_MAX) {
          PyErr_Format(PyExc_ValueError, "string too long for python");
          return NULL;
        }

        return PyString_FromStringAndSize(start, (long)len);
      }

    case BSER_ARRAY:
      return bunser_array(ptr, end, mutable);

    case BSER_OBJECT:
      return bunser_object(ptr, end, mutable);

    case BSER_TEMPLATE:
      return bunser_template(ptr, end, mutable);

    default:
      PyErr_Format(PyExc_ValueError, "unhandled bser opcode 0x%02x", buf[0]);
  }

  return NULL;
}

// Expected use case is to read a packet from the socket and
// then call bser.pdu_len on the packet.  It returns the total
// length of the entire response that the peer is sending,
// including the bytes already received.  This allows the client
// to compute the data size it needs to read before it can
// decode the data
static PyObject *bser_pdu_len(PyObject *self, PyObject *args)
{
  const char *start = NULL;
  const char *data = NULL;
  int datalen = 0;
  const char *end;
  int64_t expected_len, total_len;

  if (!PyArg_ParseTuple(args, "s#", &start, &datalen)) {
    return NULL;
  }
  data = start;
  end = data + datalen;

  // Validate the header and length
  if (memcmp(data, EMPTY_HEADER, 2) != 0) {
    PyErr_SetString(PyExc_ValueError, "invalid bser header");
    return NULL;
  }

  data += 2;

  // Expect an integer telling us how big the rest of the data
  // should be
  if (!bunser_int(&data, end, &expected_len)) {
    return NULL;
  }

  total_len = expected_len + (data - start);
  if (total_len > LONG_MAX) {
    return PyLong_FromLongLong(total_len);
  }
  return PyInt_FromLong((long)total_len);
}

static PyObject *bser_loads(PyObject *self, PyObject *args)
{
  const char *data = NULL;
  int datalen = 0;
  const char *end;
  int64_t expected_len;
  int mutable = 1;
  PyObject *mutable_obj = NULL;

  if (!PyArg_ParseTuple(args, "s#|O:loads", &data, &datalen, &mutable_obj)) {
    return NULL;
  }
  if (mutable_obj) {
    mutable = PyObject_IsTrue(mutable_obj) > 0 ? 1 : 0;
  }

  end = data + datalen;

  // Validate the header and length
  if (memcmp(data, EMPTY_HEADER, 2) != 0) {
    PyErr_SetString(PyExc_ValueError, "invalid bser header");
    return NULL;
  }

  data += 2;

  // Expect an integer telling us how big the rest of the data
  // should be
  if (!bunser_int(&data, end, &expected_len)) {
    return NULL;
  }

  // Verify
  if (expected_len + data != end) {
    PyErr_SetString(PyExc_ValueError, "bser data len != header len");
    return NULL;
  }

  return bser_loads_recursive(&data, end, mutable);
}

static PyMethodDef bser_methods[] = {
  {"loads",  bser_loads, METH_VARARGS, "Deserialize string."},
  {"pdu_len", bser_pdu_len, METH_VARARGS, "Extract PDU length."},
  {"dumps",  bser_dumps, METH_VARARGS, "Serialize string."},
  {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC initbser(void)
{
  (void)Py_InitModule("bser", bser_methods);
  PyType_Ready(&bserObjectType);
}

/* vim:ts=2:sw=2:et:
 */
