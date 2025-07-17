#include <jni.h>
#include <android/log.h>
#include <sentry.h>

#define TAG "ndk-sample"

extern "C" {

JNIEXPORT void JNICALL Java_io_sentry_ndk_sample_NdkSample_crash(JNIEnv *env, jclass cls) {
    __android_log_print(ANDROID_LOG_WARN, TAG, "About to crash.");
    char *ptr = 0;
    *ptr += 1;
}

JNIEXPORT void JNICALL Java_io_sentry_ndk_sample_NdkSample_message(JNIEnv *env, jclass cls) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "Sending message.");
    sentry_value_t event = sentry_value_new_message_event(
            /*   level */ SENTRY_LEVEL_INFO,
            /*  logger */ "custom",
            /* message */ "It works!"
    );
    sentry_capture_event(event);
}

JNIEXPORT void JNICALL Java_io_sentry_ndk_sample_NdkSample_transaction(JNIEnv *env, jclass cls) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "Sending transaction.");

    sentry_transaction_context_t *tx_ctx
            = sentry_transaction_context_new("little.teapot",
                                             "Short and stout here is my handle and here is my spout");
    sentry_value_t custom_sampling_ctx = sentry_value_new_object();
    sentry_value_set_by_key(
            custom_sampling_ctx, "b", sentry_value_new_int32(42));
    sentry_transaction_t *tx
            = sentry_transaction_start(tx_ctx, custom_sampling_ctx);
    sentry_transaction_set_data(
            tx, "url", sentry_value_new_string("https://example.com"));
    sentry_span_t *child
            = sentry_transaction_start_child(tx, "littler.teapot", NULL);
    sentry_span_t *grandchild
            = sentry_span_start_child(child, "littlest.teapot", NULL);

    sentry_span_set_data(
            child, "span_data_says", sentry_value_new_string("hi!"));
    sentry_span_finish(grandchild);
    sentry_span_finish(child);
    sentry_uuid_s uuid = sentry_transaction_finish(tx);
    if(sentry_uuid_is_nil(&uuid)) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Transaction was not sent.");
    } else {
        __android_log_print(ANDROID_LOG_INFO, TAG, "Transaction sent.");

    }

}
}
