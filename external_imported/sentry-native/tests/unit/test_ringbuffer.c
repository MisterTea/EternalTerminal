#include "sentry_ringbuffer.h"
#include "sentry_testsupport.h"
#include "sentry_value.h"

#define CHECK_KEY_IDX(List, Idx, Key, Val)                                     \
    TEST_CHECK_INT_EQUAL(sentry_value_as_int32(sentry_value_get_by_key(        \
                             sentry_value_get_by_index(List, Idx), Key)),      \
        Val)

#define CHECK_IDX(List, Idx, Val)                                              \
    TEST_CHECK_INT_EQUAL(                                                      \
        sentry_value_as_int32(sentry_value_get_by_index(List, Idx)), Val)

SENTRY_TEST(ringbuffer_append)
{
    sentry_ringbuffer_t *rb = sentry__ringbuffer_new(5);
    TEST_ASSERT(!!rb);
    for (int32_t i = 1; i <= 10; i++) {
        sentry__ringbuffer_append(rb, sentry_value_new_int32(i));
    }
    sentry__ringbuffer_append(rb, sentry_value_new_int32(1010));
    CHECK_IDX(rb->list, 0, 1010);
    CHECK_IDX(rb->list, 1, 7);
    CHECK_IDX(rb->list, 2, 8);
    CHECK_IDX(rb->list, 3, 9);
    CHECK_IDX(rb->list, 4, 10);
    sentry__ringbuffer_free(rb);
}

SENTRY_TEST(ringbuffer_append_value_refcount)
{
    const sentry_value_t v0 = sentry_value_new_object();
    sentry_value_set_by_key(v0, "key", sentry_value_new_int32((int32_t)0));
    const sentry_value_t v1 = sentry_value_new_object();
    sentry_value_set_by_key(v1, "key", sentry_value_new_int32((int32_t)1));
    const sentry_value_t v2 = sentry_value_new_object();
    sentry_value_set_by_key(v2, "key", sentry_value_new_int32((int32_t)2));
    const sentry_value_t v3 = sentry_value_new_object();
    sentry_value_set_by_key(v3, "key", sentry_value_new_int32((int32_t)3));

    sentry_ringbuffer_t *rb = sentry__ringbuffer_new(3);
    TEST_ASSERT(!!rb);
    sentry__ringbuffer_append(rb, v0);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v0), 1);
    sentry_value_incref(v0);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v0), 2);

    sentry__ringbuffer_append(rb, v1);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v1), 1);
    sentry__ringbuffer_append(rb, v2);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v2), 1);
    sentry__ringbuffer_append(rb, v3);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v3), 1);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v0), 1);

    const sentry_value_t l = sentry__ringbuffer_to_list(rb);
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(l), 3);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v3), 2);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v2), 2);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v1), 2);

    CHECK_KEY_IDX(l, 0, "key", 1);
    CHECK_KEY_IDX(l, 1, "key", 2);
    CHECK_KEY_IDX(l, 2, "key", 3);

    sentry_value_decref(l);
    sentry__ringbuffer_free(rb);
    sentry_value_decref(v0); // one manual incref
}

SENTRY_TEST(ringbuffer_free_null_noop)
{
    // freeing a NULL ringbuffer is a noop, but safe to do.
    sentry__ringbuffer_free(NULL);
}

SENTRY_TEST(ringbuffer_append_null_decref_value)
{
    // a value appended to a NULL ringbuffer will be decrefed
    sentry_value_t v = sentry_value_new_object();
    sentry_value_incref(v);
    int rv = sentry__ringbuffer_append(NULL, v);
    TEST_CHECK_INT_EQUAL(rv, -1);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v), 1);
    sentry_value_decref(v);
}

SENTRY_TEST(ringbuffer_zero_noop)
{
    sentry_ringbuffer_t *rb = sentry__ringbuffer_new(0);
    sentry_value_t v = sentry_value_new_object();
    sentry_value_incref(v);

    int rv = sentry__ringbuffer_append(rb, v);
    TEST_CHECK_INT_EQUAL(rv, -1);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v), 1);
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(rb->list), 0);

    sentry_value_decref(v);
    sentry__ringbuffer_free(rb);
}

SENTRY_TEST(ringbuffer_append_invalid_decref_value)
{
    // A ringbuffer in an invalid state, also decrefs the to-append value
    sentry_ringbuffer_t *rb = sentry__ringbuffer_new(3);

    sentry_value_t v1 = sentry_value_new_object();
    sentry__ringbuffer_append(rb, v1);

    sentry_value_t v2 = sentry_value_new_object();
    sentry_value_incref(v2);

    // this is against the contract, but only simulates an invalid state
    rb->max_size = 0;
    int rv = sentry__ringbuffer_append(rb, v2);
    TEST_CHECK_INT_EQUAL(rv, -1);
    TEST_CHECK_INT_EQUAL(sentry_value_refcount(v2), 1);
    // v2 must be manually freed
    sentry_value_decref(v2);

    // v1 will be freed from the ringbuffer
    sentry__ringbuffer_free(rb);
}

SENTRY_TEST(ringbuffer_to_list_null_value_null)
{
    // A NULL ringbuffer will produce a null value when converted to a list.
    sentry_value_t rv = sentry__ringbuffer_to_list(NULL);
    TEST_CHECK(sentry_value_is_null(rv));
}

SENTRY_TEST(ringbuffer_max_size_null_noop)
{
    // Setting a max-size for a NULL ringbuffer is a noop.
    sentry__ringbuffer_set_max_size(NULL, 100);
}

SENTRY_TEST(ringbuffer_max_size_post_init)
{
    // Setting a max-size before elements have been appended, updates the
    // max_size from the initialization.
    sentry_ringbuffer_t *rb = sentry__ringbuffer_new(1);
    sentry__ringbuffer_set_max_size(rb, 2);

    sentry_value_t v1 = sentry_value_new_int32(1);
    sentry__ringbuffer_append(rb, v1);
    sentry_value_t v2 = sentry_value_new_int32(2);
    sentry__ringbuffer_append(rb, v2);

    sentry_value_t list = sentry__ringbuffer_to_list(rb);
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(list), 2);
    CHECK_IDX(list, 0, 1);
    CHECK_IDX(list, 1, 2);
    sentry_value_decref(list);

    sentry__ringbuffer_free(rb);
}

SENTRY_TEST(ringbuffer_max_size_nonempty_noop)
{
    // Setting a max-size for a non-empty ringbuffer is a noop.
    sentry_ringbuffer_t *rb = sentry__ringbuffer_new(1);

    sentry_value_t v1 = sentry_value_new_int32(1);
    sentry__ringbuffer_append(rb, v1);

    // this is a noop, because we already appended
    sentry__ringbuffer_set_max_size(rb, 100);

    // this overwrites v1 inside the ringbuffer
    sentry_value_t v2 = sentry_value_new_int32(2);
    sentry__ringbuffer_append(rb, v2);

    sentry_value_t list = sentry__ringbuffer_to_list(rb);
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(list), 1);
    CHECK_IDX(list, 0, 2);
    sentry_value_decref(list);

    sentry__ringbuffer_free(rb);
}
