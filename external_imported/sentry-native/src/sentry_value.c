#include "sentry_boot.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4127) // conditional expression is constant
#endif

#include "../vendor/mpack.h"

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_json.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_utils.h"
#include "sentry_value.h"

/**
 * Pointer Tagging of `sentry_value_t`
 *
 * We expect all of the pointers we deal with to be at least 4-byte aligned,
 * which means we can use the least significant 2 bits for tagging.
 * We only ever save pointers to `thing_t`, which has an `alignof >= 4`, and
 * also both our own allocator, and the system allocator should give us
 * properly aligned pointers.
 *
 * xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxx00
 *                                                                      ||
 *               Pointer to a `thing_t`, a refcounted heap allocation - 00
 * xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx -   A `int32_t` shifted by 32  - 01
 *                                                     CONST as below - 10
 *                                                            false - 0010
 *                                                             true - 0110
 *                                                             null - 1010
 */

#define TAG_MASK 0x3
#define TAG_INT32 0x1
#define TAG_CONST 0x2

#define CONST_FALSE 0x2
#define CONST_TRUE 0x6
#define CONST_NULL 0xa

#define THING_TYPE_MASK 0x7f
#define THING_TYPE_FROZEN 0x80
#define THING_TYPE_LIST 0
#define THING_TYPE_OBJECT 1
#define THING_TYPE_STRING 2
#define THING_TYPE_DOUBLE 3

/* internal value helpers */

typedef struct {
    union {
        void *_ptr;
        double _double;
    } payload;
    long refcount;
    uint8_t type;
} thing_t;

typedef struct {
    sentry_value_t *items;
    size_t len;
    size_t allocated;
} list_t;

typedef struct {
    char *k;
    sentry_value_t v;
} obj_pair_t;

typedef struct {
    obj_pair_t *pairs;
    size_t len;
    size_t allocated;
} obj_t;

static const char *
level_as_string(sentry_level_t level)
{
    switch (level) {
    case SENTRY_LEVEL_DEBUG:
        return "debug";
    case SENTRY_LEVEL_WARNING:
        return "warning";
    case SENTRY_LEVEL_ERROR:
        return "error";
    case SENTRY_LEVEL_FATAL:
        return "fatal";
    case SENTRY_LEVEL_INFO:
    default:
        return "info";
    }
}

static bool
reserve(void **buf, size_t item_size, size_t *allocated, size_t min_len)
{
    if (*allocated >= min_len) {
        return true;
    }
    size_t new_allocated = *allocated;
    if (new_allocated == 0) {
        new_allocated = 16;
    }
    while (new_allocated < min_len) {
        new_allocated *= 2;
    }

    void *new_buf = sentry_malloc(new_allocated * item_size);
    if (!new_buf) {
        return false;
    }

    if (*buf) {
        memcpy(new_buf, *buf, *allocated * item_size);
        sentry_free(*buf);
    }
    *buf = new_buf;
    *allocated = new_allocated;
    return true;
}

static int
thing_get_type(const thing_t *thing)
{
    return thing->type & (uint8_t)THING_TYPE_MASK;
}

static void
thing_free(thing_t *thing)
{
    switch (thing_get_type(thing)) {
    case THING_TYPE_LIST: {
        list_t *list = thing->payload._ptr;
        for (size_t i = 0; i < list->len; i++) {
            sentry_value_decref(list->items[i]);
        }
        sentry_free(list->items);
        sentry_free(list);
        break;
    }
    case THING_TYPE_OBJECT: {
        obj_t *obj = thing->payload._ptr;
        for (size_t i = 0; i < obj->len; i++) {
            sentry_free(obj->pairs[i].k);
            sentry_value_decref(obj->pairs[i].v);
        }
        sentry_free(obj->pairs);
        sentry_free(obj);
        break;
    }
    case THING_TYPE_STRING: {
        sentry_free(thing->payload._ptr);
        break;
    }
    }
    sentry_free(thing);
}

static int
thing_is_frozen(const thing_t *thing)
{
    return (thing->type >> 7) != 0;
}

static void
thing_freeze(thing_t *thing)
{
    if (thing_is_frozen(thing)) {
        return;
    }
    thing->type |= 0x80;
    switch (thing_get_type(thing)) {
    case THING_TYPE_LIST: {
        const list_t *l = thing->payload._ptr;
        for (size_t i = 0; i < l->len; i++) {
            sentry_value_freeze(l->items[i]);
        }
        break;
    }
    case THING_TYPE_OBJECT: {
        const obj_t *o = thing->payload._ptr;
        for (size_t i = 0; i < o->len; i++) {
            sentry_value_freeze(o->pairs[i].v);
        }
        break;
    }
    default:;
    }
}

static sentry_value_t
new_thing_value(void *ptr, uint8_t thing_type)
{
    thing_t *thing = sentry_malloc(sizeof(thing_t));
    if (!thing) {
        return sentry_value_new_null();
    }
    thing->payload._ptr = ptr;
    thing->refcount = 1;
    thing->type = thing_type;

    sentry_value_t rv;
    rv._bits = (uint64_t)(size_t)thing;
    return rv;
}

static thing_t *
value_as_thing(sentry_value_t value)
{
    if (value._bits & TAG_MASK) {
        return NULL;
    }
    return (thing_t *)(size_t)value._bits;
}

static thing_t *
value_as_unfrozen_thing(sentry_value_t value)
{
    thing_t *thing = value_as_thing(value);
    return thing && !thing_is_frozen(thing) ? thing : NULL;
}

/* public api implementations */

void
sentry_value_incref(sentry_value_t value)
{
    thing_t *thing = value_as_thing(value);
    if (thing) {
        sentry__atomic_fetch_and_add(&thing->refcount, 1);
    }
}

void
sentry_value_decref(sentry_value_t value)
{
    thing_t *thing = value_as_thing(value);
    if (thing && sentry__atomic_fetch_and_add(&thing->refcount, -1) == 1) {
        thing_free(thing);
    }
}

size_t
sentry_value_refcount(sentry_value_t value)
{
    thing_t *thing = value_as_thing(value);
    return thing ? (size_t)sentry__atomic_fetch(&thing->refcount) : 1;
}

void
sentry_value_freeze(sentry_value_t value)
{
    thing_t *thing = value_as_thing(value);
    if (thing) {
        thing_freeze(thing);
    }
}

int
sentry_value_is_frozen(sentry_value_t value)
{
    const thing_t *thing = value_as_thing(value);
    return thing ? thing_is_frozen(thing) : true;
}

sentry_value_t
sentry_value_new_null(void)
{
    sentry_value_t rv;
    rv._bits = (uint64_t)CONST_NULL;
    return rv;
}

sentry_value_t
sentry_value_new_int32(int32_t value)
{
    sentry_value_t rv;
    rv._bits = ((uint64_t)(uint32_t)value) << 32 | TAG_INT32;
    return rv;
}

sentry_value_t
sentry_value_new_double(double value)
{
    thing_t *thing = sentry_malloc(sizeof(thing_t));
    if (!thing) {
        return sentry_value_new_null();
    }
    thing->payload._double = value;
    thing->refcount = 1;
    thing->type = (uint8_t)(THING_TYPE_DOUBLE | THING_TYPE_FROZEN);

    sentry_value_t rv;
    rv._bits = (uint64_t)(size_t)thing;
    return rv;
}

sentry_value_t
sentry_value_new_bool(int value)
{
    sentry_value_t rv;
    rv._bits = (uint64_t)(value ? CONST_TRUE : CONST_FALSE);
    return rv;
}

sentry_value_t
sentry_value_new_string(const char *value)
{
    char *s = sentry__string_clone(value);
    if (!s) {
        return sentry_value_new_null();
    }
    return sentry__value_new_string_owned(s);
}

sentry_value_t
sentry_value_new_list(void)
{
    list_t *l = SENTRY_MAKE(list_t);
    if (l) {
        memset(l, 0, sizeof(list_t));
        sentry_value_t rv = new_thing_value(l, THING_TYPE_LIST);
        if (sentry_value_is_null(rv)) {
            sentry_free(l);
        }
        return rv;
    } else {
        return sentry_value_new_null();
    }
}

sentry_value_t
sentry__value_new_list_with_size(size_t size)
{
    list_t *l = SENTRY_MAKE(list_t);
    if (l) {
        memset(l, 0, sizeof(list_t));
        l->allocated = size;
        if (size) {
            l->items = sentry_malloc(sizeof(sentry_value_t) * size);
            if (!l->items) {
                sentry_free(l);
                return sentry_value_new_null();
            }
        }
        sentry_value_t rv = new_thing_value(l, THING_TYPE_LIST);
        if (sentry_value_is_null(rv)) {
            sentry_free(l->items);
            sentry_free(l);
        }
        return rv;
    } else {
        return sentry_value_new_null();
    }
}

sentry_value_t
sentry_value_new_object(void)
{
    obj_t *o = SENTRY_MAKE(obj_t);
    if (o) {
        memset(o, 0, sizeof(obj_t));
        sentry_value_t rv = new_thing_value(o, THING_TYPE_OBJECT);
        if (sentry_value_is_null(rv)) {
            sentry_free(o);
        }
        return rv;
    } else {
        return sentry_value_new_null();
    }
}

sentry_value_t
sentry__value_new_object_with_size(size_t size)
{
    obj_t *o = SENTRY_MAKE(obj_t);
    if (o) {
        memset(o, 0, sizeof(obj_t));
        o->allocated = size;
        if (size) {
            o->pairs = sentry_malloc(sizeof(obj_pair_t) * size);
            if (!o->pairs) {
                sentry_free(o);
                return sentry_value_new_null();
            }
        }
        sentry_value_t rv = new_thing_value(o, THING_TYPE_OBJECT);
        if (sentry_value_is_null(rv)) {
            sentry_free(o->pairs);
            sentry_free(o);
        }
        return rv;
    } else {
        return sentry_value_new_null();
    }
}

sentry_value_type_t
sentry_value_get_type(sentry_value_t value)
{
    if (sentry_value_is_null(value)) {
        return SENTRY_VALUE_TYPE_NULL;
    }
    const thing_t *thing = value_as_thing(value);
    if (thing) {
        switch (thing_get_type(thing)) {
        case THING_TYPE_STRING:
            return SENTRY_VALUE_TYPE_STRING;
        case THING_TYPE_LIST:
            return SENTRY_VALUE_TYPE_LIST;
        case THING_TYPE_OBJECT:
            return SENTRY_VALUE_TYPE_OBJECT;
        case THING_TYPE_DOUBLE:
            return SENTRY_VALUE_TYPE_DOUBLE;
        }
        assert(!"unreachable");
    } else if ((value._bits & TAG_MASK) == TAG_CONST) {
        return SENTRY_VALUE_TYPE_BOOL;
    } else if ((value._bits & TAG_MASK) == TAG_INT32) {
        return SENTRY_VALUE_TYPE_INT32;
    }
    assert(!"unreachable");
    return SENTRY_VALUE_TYPE_NULL;
}

int
sentry_value_set_by_key(sentry_value_t value, const char *k, sentry_value_t v)
{
    thing_t *thing = value_as_unfrozen_thing(value);
    if (!thing || thing_get_type(thing) != THING_TYPE_OBJECT) {
        goto fail;
    }
    obj_t *o = thing->payload._ptr;
    for (size_t i = 0; i < o->len; i++) {
        obj_pair_t *pair = &o->pairs[i];
        if (sentry__string_eq(pair->k, k)) {
            sentry_value_decref(pair->v);
            pair->v = v;
            return 0;
        }
    }

    if (!reserve((void **)&o->pairs, sizeof(o->pairs[0]), &o->allocated,
            o->len + 1)) {
        goto fail;
    }

    obj_pair_t pair;
    pair.k = sentry__string_clone(k);
    if (!pair.k) {
        goto fail;
    }
    pair.v = v;
    o->pairs[o->len++] = pair;
    return 0;

fail:
    sentry_value_decref(v);
    return 1;
}

int
sentry_value_remove_by_key(sentry_value_t value, const char *k)
{
    thing_t *thing = value_as_unfrozen_thing(value);
    if (!thing || thing_get_type(thing) != THING_TYPE_OBJECT) {
        return 1;
    }
    obj_t *o = thing->payload._ptr;
    for (size_t i = 0; i < o->len; i++) {
        obj_pair_t *pair = &o->pairs[i];
        if (sentry__string_eq(pair->k, k)) {
            sentry_free(pair->k);
            sentry_value_decref(pair->v);
            memmove(o->pairs + i, o->pairs + i + 1,
                (o->len - i - 1) * sizeof(o->pairs[0]));
            o->len--;
            return 0;
        }
    }
    return 1;
}

int
sentry_value_append(sentry_value_t value, sentry_value_t v)
{
    thing_t *thing = value_as_unfrozen_thing(value);
    if (!thing || thing_get_type(thing) != THING_TYPE_LIST) {
        goto fail;
    }

    list_t *l = thing->payload._ptr;

    if (!reserve((void **)&l->items, sizeof(l->items[0]), &l->allocated,
            l->len + 1)) {
        goto fail;
    }

    l->items[l->len++] = v;
    return 0;

fail:
    sentry_value_decref(v);
    return 1;
}

sentry_uuid_t
sentry__value_as_uuid(sentry_value_t value)
{
    const char *val = sentry_value_as_string(value);
    if (val) {
        return sentry_uuid_from_string(val);
    } else {
        return sentry_uuid_nil();
    }
}

char *
sentry__value_stringify(sentry_value_t value)
{
    switch (sentry_value_get_type(value)) {
    case SENTRY_VALUE_TYPE_LIST:
    case SENTRY_VALUE_TYPE_OBJECT:
    case SENTRY_VALUE_TYPE_NULL:
        return sentry__string_clone("");
    case SENTRY_VALUE_TYPE_BOOL:
        return sentry__string_clone(
            sentry_value_is_true(value) ? "true" : "false");
    case SENTRY_VALUE_TYPE_STRING:
        return sentry__string_clone(sentry_value_as_string(value));
    default: {
        char buf[50];
        snprintf(buf, sizeof(buf), "%g", sentry_value_as_double(value));
        return sentry__string_clone(buf);
    }
    }
}

sentry_value_t
sentry__value_clone(sentry_value_t value)
{
    const thing_t *thing = value_as_thing(value);
    if (!thing) {
        return value;
    }
    switch (thing_get_type(thing)) {
    case THING_TYPE_LIST: {
        const list_t *list = thing->payload._ptr;
        sentry_value_t rv = sentry__value_new_list_with_size(list->len);
        for (size_t i = 0; i < list->len; i++) {
            sentry_value_incref(list->items[i]);
            sentry_value_append(rv, list->items[i]);
        }
        return rv;
    }
    case THING_TYPE_OBJECT: {
        const obj_t *obj = thing->payload._ptr;
        sentry_value_t rv = sentry__value_new_object_with_size(obj->len);
        for (size_t i = 0; i < obj->len; i++) {
            sentry_value_incref(obj->pairs[i].v);
            sentry_value_set_by_key(rv, obj->pairs[i].k, obj->pairs[i].v);
        }
        return rv;
    }
    case THING_TYPE_STRING:
    case THING_TYPE_DOUBLE:
        sentry_value_incref(value);
        return value;
    default:
        return sentry_value_new_null();
    }
}

int
sentry__value_append_bounded(sentry_value_t value, sentry_value_t v, size_t max)
{
    thing_t *thing = value_as_unfrozen_thing(value);
    if (!thing || thing_get_type(thing) != THING_TYPE_LIST) {
        goto fail;
    }

    list_t *l = thing->payload._ptr;

    if (l->len < max) {
        return sentry_value_append(value, v);
    }

    // len: 120
    // max: 100
    // move to 0
    //   move 99 items (len - 1)
    //   from 20

    size_t to_move = max - 1;
    size_t to_shift = l->len - to_move;
    for (size_t i = 0; i < to_shift; i++) {
        sentry_value_decref(l->items[i]);
    }
    memmove(l->items, l->items + (to_shift), to_move * sizeof(l->items[0]));
    l->items[max - 1] = v;
    l->len = max;
    return 0;

fail:
    sentry_value_decref(v);
    return 1;
}

int
sentry_value_set_by_index(sentry_value_t value, size_t index, sentry_value_t v)
{
    thing_t *thing = value_as_unfrozen_thing(value);
    if (!thing || thing_get_type(thing) != THING_TYPE_LIST) {
        goto fail;
    }

    list_t *l = thing->payload._ptr;
    if (!reserve(
            (void *)&l->items, sizeof(l->items[0]), &l->allocated, index + 1)) {
        goto fail;
    }

    if (index >= l->len) {
        for (size_t i = l->len; i < index + 1; i++) {
            l->items[i] = sentry_value_new_null();
        }
        l->len = index + 1;
    }

    sentry_value_decref(l->items[index]);
    l->items[index] = v;
    return 0;

fail:
    sentry_value_decref(v);
    return 1;
}

int
sentry_value_remove_by_index(sentry_value_t value, size_t index)
{
    thing_t *thing = value_as_unfrozen_thing(value);
    if (!thing || thing_get_type(thing) != THING_TYPE_LIST) {
        return 1;
    }

    list_t *l = thing->payload._ptr;
    if (index >= l->len) {
        return 0;
    }

    sentry_value_decref(l->items[index]);
    memmove(l->items + index, l->items + index + 1,
        (l->len - index - 1) * sizeof(l->items[0]));
    l->len--;
    return 0;
}

sentry_value_t
sentry_value_get_by_key(sentry_value_t value, const char *k)
{
    const thing_t *thing = value_as_thing(value);
    if (thing && thing_get_type(thing) == THING_TYPE_OBJECT) {
        obj_t *o = thing->payload._ptr;
        for (size_t i = 0; i < o->len; i++) {
            obj_pair_t *pair = &o->pairs[i];
            if (sentry__string_eq(pair->k, k)) {
                return pair->v;
            }
        }
    }
    return sentry_value_new_null();
}

sentry_value_t
sentry_value_get_by_key_owned(sentry_value_t value, const char *k)
{
    sentry_value_t rv = sentry_value_get_by_key(value, k);
    sentry_value_incref(rv);
    return rv;
}

sentry_value_t
sentry_value_get_by_index(sentry_value_t value, size_t index)
{
    const thing_t *thing = value_as_thing(value);
    if (thing && thing_get_type(thing) == THING_TYPE_LIST) {
        list_t *l = thing->payload._ptr;
        if (index < l->len) {
            return l->items[index];
        }
    }
    return sentry_value_new_null();
}

sentry_value_t
sentry_value_get_by_index_owned(sentry_value_t value, size_t index)
{
    sentry_value_t rv = sentry_value_get_by_index(value, index);
    sentry_value_incref(rv);
    return rv;
}

size_t
sentry_value_get_length(sentry_value_t value)
{
    const thing_t *thing = value_as_thing(value);
    if (thing) {
        switch (thing_get_type(thing)) {
        case THING_TYPE_STRING:
            return strlen(thing->payload._ptr);
        case THING_TYPE_LIST:
            return ((const list_t *)thing->payload._ptr)->len;
        case THING_TYPE_OBJECT:
            return ((const obj_t *)thing->payload._ptr)->len;
        }
    }
    return 0;
}

int32_t
sentry_value_as_int32(sentry_value_t value)
{
    if ((value._bits & TAG_MASK) == TAG_INT32) {
        return (int32_t)((int64_t)value._bits >> 32);
    } else {
        return 0;
    }
}

double
sentry_value_as_double(sentry_value_t value)
{
    if ((value._bits & TAG_MASK) == TAG_INT32) {
        return (double)sentry_value_as_int32(value);
    }

    const thing_t *thing = value_as_thing(value);
    if (thing && thing_get_type(thing) == THING_TYPE_DOUBLE) {
        return thing->payload._double;
    } else {
        return NAN;
    }
}

const char *
sentry_value_as_string(sentry_value_t value)
{
    const thing_t *thing = value_as_thing(value);
    if (thing && thing_get_type(thing) == THING_TYPE_STRING) {
        return (const char *)thing->payload._ptr;
    } else {
        return "";
    }
}

int
sentry_value_is_true(sentry_value_t value)
{
    if (value._bits == CONST_TRUE) {
        return 1;
    }
    switch (sentry_value_get_type(value)) {
    case SENTRY_VALUE_TYPE_BOOL:
    case SENTRY_VALUE_TYPE_NULL:
        return 0;
    case SENTRY_VALUE_TYPE_INT32:
        return sentry_value_as_int32(value) != 0;
    case SENTRY_VALUE_TYPE_DOUBLE:
        return sentry_value_as_double(value) != 0.0;
    default:
        return sentry_value_get_length(value) > 0;
    }
}

int
sentry_value_is_null(sentry_value_t value)
{
    return value._bits == CONST_NULL;
}

static void
value_to_json(sentry_jsonwriter_t *jw, sentry_value_t value)
{
    switch (sentry_value_get_type(value)) {
    case SENTRY_VALUE_TYPE_NULL:
        sentry__jsonwriter_write_null(jw);
        break;
    case SENTRY_VALUE_TYPE_BOOL:
        sentry__jsonwriter_write_bool(jw, sentry_value_is_true(value));
        break;
    case SENTRY_VALUE_TYPE_INT32:
        sentry__jsonwriter_write_int32(jw, sentry_value_as_int32(value));
        break;
    case SENTRY_VALUE_TYPE_DOUBLE:
        sentry__jsonwriter_write_double(jw, sentry_value_as_double(value));
        break;
    case SENTRY_VALUE_TYPE_STRING:
        sentry__jsonwriter_write_str(jw, sentry_value_as_string(value));
        break;
    case SENTRY_VALUE_TYPE_LIST: {
        const list_t *l = value_as_thing(value)->payload._ptr;
        sentry__jsonwriter_write_list_start(jw);
        for (size_t i = 0; i < l->len; i++) {
            value_to_json(jw, l->items[i]);
        }
        sentry__jsonwriter_write_list_end(jw);
        break;
    }
    case SENTRY_VALUE_TYPE_OBJECT: {
        const obj_t *o = value_as_thing(value)->payload._ptr;
        sentry__jsonwriter_write_object_start(jw);
        for (size_t i = 0; i < o->len; i++) {
            sentry__jsonwriter_write_key(jw, o->pairs[i].k);
            value_to_json(jw, o->pairs[i].v);
        }
        sentry__jsonwriter_write_object_end(jw);
        break;
    }
    }
}

char *
sentry_value_to_json(sentry_value_t value)
{
    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_in_memory();
    value_to_json(jw, value);
    return sentry__jsonwriter_into_string(jw, NULL);
}

static void
value_to_msgpack(mpack_writer_t *writer, sentry_value_t value)
{
    switch (sentry_value_get_type(value)) {
    case SENTRY_VALUE_TYPE_NULL:
        mpack_write_nil(writer);
        break;
    case SENTRY_VALUE_TYPE_BOOL:
        mpack_write_bool(writer, sentry_value_is_true(value) ? true : false);
        break;
    case SENTRY_VALUE_TYPE_INT32:
        mpack_write_i32(writer, sentry_value_as_int32(value));
        break;
    case SENTRY_VALUE_TYPE_DOUBLE:
        mpack_write_double(writer, sentry_value_as_double(value));
        break;
    case SENTRY_VALUE_TYPE_STRING: {
        mpack_write_cstr_or_nil(writer, sentry_value_as_string(value));
        break;
    }
    case SENTRY_VALUE_TYPE_LIST: {
        const list_t *l = value_as_thing(value)->payload._ptr;

        mpack_start_array(writer, (uint32_t)l->len);
        for (size_t i = 0; i < l->len; i++) {
            value_to_msgpack(writer, l->items[i]);
        }
        mpack_finish_array(writer);
        break;
    }
    case SENTRY_VALUE_TYPE_OBJECT: {
        const obj_t *o = value_as_thing(value)->payload._ptr;

        mpack_start_map(writer, (uint32_t)o->len);
        for (size_t i = 0; i < o->len; i++) {
            mpack_write_cstr(writer, o->pairs[i].k);
            value_to_msgpack(writer, o->pairs[i].v);
        }
        mpack_finish_map(writer);
        break;
    }
    }
}

char *
sentry_value_to_msgpack(sentry_value_t value, size_t *size_out)
{
    mpack_writer_t writer;
    char *buf;
    size_t size;
    mpack_writer_init_growable(&writer, &buf, &size);
    value_to_msgpack(&writer, value);
    mpack_writer_destroy(&writer);
    *size_out = size;
    return buf;
}

sentry_value_t
sentry__value_new_string_owned(char *s)
{
    sentry_value_t rv
        = new_thing_value(s, THING_TYPE_STRING | THING_TYPE_FROZEN);
    if (sentry_value_is_null(rv)) {
        sentry_free(s);
    }
    return rv;
}

#ifdef SENTRY_PLATFORM_WINDOWS
sentry_value_t
sentry__value_new_string_from_wstr(const wchar_t *s)
{
    char *rv = sentry__string_from_wstr(s);
    return rv ? sentry__value_new_string_owned(rv) : sentry_value_new_null();
}
#endif

sentry_value_t
sentry__value_new_addr(uint64_t addr)
{
    char buf[100];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)addr);
    return sentry_value_new_string(buf);
}

sentry_value_t
sentry__value_new_hexstring(const uint8_t *bytes, size_t len)
{
    char *buf = sentry_malloc(len * 2 + 1);
    if (!buf) {
        return sentry_value_new_null();
    }
    char *ptr = buf;
    for (size_t i = 0; i < len; i++) {
        ptr += snprintf(ptr, 3, "%02hhx", bytes[i]);
    }
    return sentry__value_new_string_owned(buf);
}

sentry_value_t
sentry__value_new_uuid(const sentry_uuid_t *uuid)
{
    char *buf = sentry_malloc(37);
    if (!buf) {
        return sentry_value_new_null();
    }
    sentry_uuid_as_string(uuid, buf);
    return sentry__value_new_string_owned(buf);
}

sentry_value_t
sentry__value_new_level(sentry_level_t level)
{
    return sentry_value_new_string(level_as_string(level));
}

sentry_value_t
sentry_value_new_event(void)
{
    sentry_value_t rv = sentry_value_new_object();

    sentry_uuid_t uuid = sentry__new_event_id();
    sentry_value_set_by_key(rv, "event_id", sentry__value_new_uuid(&uuid));

    sentry_value_set_by_key(rv, "timestamp",
        sentry__value_new_string_owned(
            sentry__msec_time_to_iso8601(sentry__msec_time())));

    return rv;
}

sentry_value_t
sentry_value_new_message_event(
    sentry_level_t level, const char *logger, const char *text)
{
    sentry_value_t rv = sentry_value_new_event();
    sentry_value_set_by_key(rv, "level", sentry__value_new_level(level));
    if (logger) {
        sentry_value_set_by_key(rv, "logger", sentry_value_new_string(logger));
    }
    if (text) {
        sentry_value_t container = sentry_value_new_object();
        sentry_value_set_by_key(
            container, "formatted", sentry_value_new_string(text));
        sentry_value_set_by_key(rv, "message", container);
    }
    return rv;
}

sentry_value_t
sentry_value_new_breadcrumb(const char *type, const char *message)
{
    sentry_value_t rv = sentry_value_new_object();
    sentry_value_set_by_key(rv, "timestamp",
        sentry__value_new_string_owned(
            sentry__msec_time_to_iso8601(sentry__msec_time())));

    if (type) {
        sentry_value_set_by_key(rv, "type", sentry_value_new_string(type));
    }
    if (message) {
        sentry_value_set_by_key(
            rv, "message", sentry_value_new_string(message));
    }
    return rv;
}

void
sentry_event_value_add_stacktrace(sentry_value_t event, void **ips, size_t len)
{
    void *walked_backtrace[256];

    // if nobody gave us a backtrace, walk now.
    if (!ips) {
        len = sentry_unwind_stack(NULL, walked_backtrace, 256);
        ips = walked_backtrace;
    }

    sentry_value_t frames = sentry__value_new_list_with_size(len);
    for (size_t i = 0; i < len; i++) {
        sentry_value_t frame = sentry_value_new_object();
        sentry_value_set_by_key(frame, "instruction_addr",
            sentry__value_new_addr((uint64_t)(size_t)ips[len - i - 1]));
        sentry_value_append(frames, frame);
    }

    sentry_value_t stacktrace = sentry_value_new_object();
    sentry_value_set_by_key(stacktrace, "frames", frames);

    sentry_value_t thread = sentry_value_new_object();
    sentry_value_set_by_key(thread, "stacktrace", stacktrace);

    sentry_value_t values = sentry_value_new_list();
    sentry_value_append(values, thread);

    sentry_value_t threads = sentry_value_new_object();
    sentry_value_set_by_key(threads, "values", values);

    sentry_value_set_by_key(event, "threads", threads);
}
