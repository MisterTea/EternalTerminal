#include "sentry_session_replay.h"

#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_json.h"
#include "sentry_string.h"
#include "sentry_utils.h"
#include "sentry_value.h"

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4127) // conditional expression is constant
#    if defined(__clang__) // clang-cl
#        pragma clang diagnostic push
#        pragma clang diagnostic ignored "-Wdocumentation"
#        pragma clang diagnostic ignored "-Wpre-c11-compat"
#    endif
#elif defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wstatic-in-inline"
#endif

#include "../vendor/mpack.h"

#if defined(_MSC_VER)
#    pragma warning(pop)
#    ifdef __clang__ // clang-cl
#        pragma clang diagnostic pop
#    endif
#elif defined(__clang__)
#    pragma clang diagnostic pop
#endif

#include <stdio.h>

sentry_path_t *
sentry__session_replay_get_path(const sentry_options_t *options)
{
    return sentry__path_join_str(options->run->run_path, "session-replay.mp4");
}

static sentry_path_t *
session_replay_dir(const sentry_options_t *options)
{
    if (!options->run || !options->run->run_path) {
        return NULL;
    }
    sentry_path_t *db_dir = sentry__path_dir(options->run->run_path);
    if (!db_dir) {
        return NULL;
    }
    sentry_path_t *replays = sentry__path_join_str(db_dir, "replays");
    sentry__path_free(db_dir);
    return replays;
}

// Build the replay_event from the recorder's metadata. When `scope_source` (the
// crash event) is non-null, its scope fields and trace id are copied onto the
// replay so it shares the crash's context. `error_ids` is omitted (deprecated).
static sentry_value_t
build_replay_event(sentry_value_t meta, const char *replay_id, double start_sec,
    double end_sec, int32_t segment_id, sentry_value_t scope_source)
{
    const char *replay_type
        = sentry_value_as_string(sentry_value_get_by_key(meta, "replayType"));

    sentry_value_t event = sentry_value_new_object();
    sentry_value_set_by_key(
        event, "type", sentry_value_new_string("replay_event"));
    sentry_value_set_by_key(event, "replay_type",
        sentry_value_new_string(
            replay_type && replay_type[0] ? replay_type : "buffer"));
    sentry_value_set_by_key(
        event, "segment_id", sentry_value_new_int32(segment_id));
    sentry_value_set_by_key(
        event, "replay_id", sentry_value_new_string(replay_id));
    sentry_value_set_by_key(
        event, "event_id", sentry_value_new_string(replay_id));
    sentry_value_set_by_key(
        event, "platform", sentry_value_new_string("native"));
    sentry_value_set_by_key(
        event, "timestamp", sentry_value_new_double(end_sec));
    sentry_value_set_by_key(
        event, "replay_start_timestamp", sentry_value_new_double(start_sec));
    sentry_value_set_by_key(event, "urls", sentry_value_new_list());

    if (!sentry_value_is_null(scope_source)) {
        static const char *const scope_keys[] = { "tags", "contexts", "release",
            "environment", "dist", "user", "sdk" };
        for (size_t i = 0; i < sizeof(scope_keys) / sizeof(scope_keys[0]);
            i++) {
            sentry_value_t v
                = sentry_value_get_by_key(scope_source, scope_keys[i]);
            if (!sentry_value_is_null(v)) {
                sentry_value_incref(v);
                sentry_value_set_by_key(event, scope_keys[i], v);
            }
        }

        sentry_value_t trace_id = sentry_value_get_by_key(
            sentry_value_get_by_key(
                sentry_value_get_by_key(scope_source, "contexts"), "trace"),
            "trace_id");
        sentry_value_t trace_ids = sentry_value_new_list();
        if (!sentry_value_is_null(trace_id)) {
            sentry_value_incref(trace_id);
            sentry_value_append(trace_ids, trace_id);
        }
        sentry_value_set_by_key(event, "trace_ids", trace_ids);
    }
    return event;
}

// Build the rrweb recording list (meta event + video event) describing the
// clip.
static sentry_value_t
build_replay_recording(sentry_value_t meta, double start_sec,
    int32_t segment_id, double size_bytes, double duration_ms)
{
    const int32_t width
        = sentry_value_as_int32(sentry_value_get_by_key(meta, "width"));
    const int32_t height
        = sentry_value_as_int32(sentry_value_get_by_key(meta, "height"));
    const double ts_ms = start_sec * 1000.0;

    sentry_value_t meta_data = sentry_value_new_object();
    sentry_value_set_by_key(meta_data, "href", sentry_value_new_string(""));
    sentry_value_set_by_key(meta_data, "width", sentry_value_new_int32(width));
    sentry_value_set_by_key(
        meta_data, "height", sentry_value_new_int32(height));
    sentry_value_t meta_event = sentry_value_new_object();
    sentry_value_set_by_key(meta_event, "type", sentry_value_new_int32(4));
    sentry_value_set_by_key(
        meta_event, "timestamp", sentry_value_new_double(ts_ms));
    sentry_value_set_by_key(meta_event, "data", meta_data);

    sentry_value_t payload = sentry_value_new_object();
    sentry_value_set_by_key(
        payload, "segmentId", sentry_value_new_int32(segment_id));
    sentry_value_set_by_key(
        payload, "size", sentry_value_new_double(size_bytes));
    sentry_value_set_by_key(
        payload, "duration", sentry_value_new_double(duration_ms));
    sentry_value_set_by_key(
        payload, "encoding", sentry_value_new_string("h264"));
    sentry_value_set_by_key(
        payload, "container", sentry_value_new_string("mp4"));
    sentry_value_set_by_key(payload, "height", sentry_value_new_int32(height));
    sentry_value_set_by_key(payload, "width", sentry_value_new_int32(width));
    sentry_value_set_by_key(payload, "left", sentry_value_new_int32(0));
    sentry_value_set_by_key(payload, "top", sentry_value_new_int32(0));
    sentry_value_set_by_key(payload, "frameCount",
        sentry_value_new_int32(sentry_value_as_int32(
            sentry_value_get_by_key(meta, "frameCount"))));
    sentry_value_set_by_key(payload, "frameRate",
        sentry_value_new_int32(
            sentry_value_as_int32(sentry_value_get_by_key(meta, "frameRate"))));
    sentry_value_set_by_key(
        payload, "frameRateType", sentry_value_new_string("variable"));
    sentry_value_t video_data = sentry_value_new_object();
    sentry_value_set_by_key(
        video_data, "tag", sentry_value_new_string("video"));
    sentry_value_set_by_key(video_data, "payload", payload);
    sentry_value_t video_event = sentry_value_new_object();
    sentry_value_set_by_key(video_event, "type", sentry_value_new_int32(5));
    sentry_value_set_by_key(
        video_event, "timestamp", sentry_value_new_double(ts_ms));
    sentry_value_set_by_key(video_event, "data", video_data);

    sentry_value_t recording = sentry_value_new_list();
    sentry_value_append(recording, meta_event);
    sentry_value_append(recording, video_event);
    return recording;
}

// Build the replay_video envelope from the parsed sidecar + its mp4. `end_sec`
// is the window end (crash time); <= 0 falls back to the sidecar's. NULL on
// failure.
static sentry_envelope_t *
build_replay_envelope(const sentry_options_t *options, sentry_value_t meta,
    const sentry_path_t *mp4_path, double end_sec, sentry_value_t scope_source)
{
    if (sentry_value_is_null(meta) || !mp4_path) {
        return NULL;
    }

    const char *replay_id
        = sentry_value_as_string(sentry_value_get_by_key(meta, "replayId"));
    if (!replay_id || !replay_id[0]) {
        return NULL;
    }

    size_t video_len = 0;
    char *video = sentry__path_read_to_buffer(mp4_path, &video_len);
    if (!video || video_len == 0) {
        sentry_free(video);
        return NULL;
    }

    const double duration_ms
        = sentry_value_as_double(sentry_value_get_by_key(meta, "durationMs"));
    if (end_sec <= 0.0) {
        end_sec = sentry_value_as_double(
            sentry_value_get_by_key(meta, "endTimestampSec"));
    }
    const double start_sec = end_sec - duration_ms / 1000.0;
    const int32_t segment_id
        = sentry_value_as_int32(sentry_value_get_by_key(meta, "segmentId"));

    sentry_value_t event = build_replay_event(
        meta, replay_id, start_sec, end_sec, segment_id, scope_source);
    sentry_value_t recording = build_replay_recording(
        meta, start_sec, segment_id, (double)video_len, duration_ms);

    sentry_envelope_t *envelope = NULL;

    size_t event_len = 0;
    char *event_json = sentry__value_to_json(event, &event_len);
    size_t rrweb_len = 0;
    char *rrweb_json = sentry__value_to_json(recording, &rrweb_len);

    if (event_json && rrweb_json) {
        // replay_recording = `{"segment_id":N}\n` + the rrweb array.
        char hdr[48];
        int hdr_len
            = snprintf(hdr, sizeof(hdr), "{\"segment_id\":%d}\n", segment_id);

        sentry_stringbuilder_t rb;
        sentry__stringbuilder_init(&rb);
        int rb_rv = sentry__stringbuilder_append_buf(&rb, hdr, (size_t)hdr_len);
        rb_rv |= sentry__stringbuilder_append_buf(&rb, rrweb_json, rrweb_len);
        size_t recording_len = sentry__stringbuilder_len(&rb);
        char *recording_buf = sentry__stringbuilder_into_string(&rb);

        if (rb_rv == 0) {
            mpack_writer_t writer;
            char *body = NULL;
            size_t body_len = 0;
            mpack_writer_init_growable(&writer, &body, &body_len);
            mpack_start_map(&writer, 3);
            mpack_write_cstr(&writer, "replay_event");
            mpack_write_bin(&writer, event_json, (uint32_t)event_len);
            mpack_write_cstr(&writer, "replay_recording");
            mpack_write_bin(&writer, recording_buf, (uint32_t)recording_len);
            mpack_write_cstr(&writer, "replay_video");
            mpack_write_bin(&writer, video, (uint32_t)video_len);
            mpack_finish_map(&writer);
            bool body_ok = mpack_writer_destroy(&writer) == mpack_ok;

            if (body_ok && body) {
                envelope = sentry__envelope_new_with_dsn(options->dsn);
                if (envelope) {
                    sentry__envelope_set_header(envelope, "event_id",
                        sentry_value_new_string(replay_id));
                    if (!sentry__envelope_add_from_buffer(
                            envelope, body, body_len, "replay_video")) {
                        sentry_envelope_free(envelope);
                        envelope = NULL;
                    }
                }
            }
            sentry_free(body);
        }

        sentry_free(recording_buf);
    }

    sentry_free(rrweb_json);
    sentry_free(event_json);
    sentry_value_decref(event);
    sentry_value_decref(recording);
    sentry_free(video);
    return envelope;
}

// Build `<dir>/replay-<replay_id><ext>`, matching the embedder's staged name.
static sentry_path_t *
replay_file_path(
    const sentry_path_t *dir, const char *replay_id, const char *ext)
{
    size_t len = strlen("replay-") + strlen(replay_id) + strlen(ext);
    char *name = sentry_malloc(len + 1);
    if (!name) {
        return NULL;
    }
    snprintf(name, len + 1, "replay-%s%s", replay_id, ext);
    sentry_path_t *path = sentry__path_join_str(dir, name);
    sentry_free(name);
    return path;
}

bool
sentry__session_replay_has_pending(const sentry_options_t *options)
{
    sentry_path_t *dir = session_replay_dir(options);
    if (!dir) {
        return false;
    }

    bool pending = false;
    sentry_pathiter_t *iter = sentry__path_iter_directory(dir);
    if (iter) {
        const sentry_path_t *entry;
        while ((entry = sentry__pathiter_next(iter)) != NULL) {
            if (sentry__path_ends_with(entry, ".mp4")) {
                pending = true;
                break;
            }
        }
        sentry__pathiter_free(iter);
    }
    sentry__path_free(dir);
    return pending;
}

void
sentry__session_replay_flush_pending(const sentry_options_t *options,
    sentry_transport_t *transport, sentry_value_t scope_source)
{
    if (!options || !transport) {
        return;
    }

    const char *replay_id = NULL;
    if (!sentry_value_is_null(scope_source)) {
        sentry_value_t replay_ctx = sentry_value_get_by_key(
            sentry_value_get_by_key(scope_source, "contexts"), "replay");
        const char *rid = sentry_value_as_string(
            sentry_value_get_by_key(replay_ctx, "replay_id"));
        if (rid && rid[0]) {
            replay_id = rid;
        }
    }
    if (!replay_id) {
        return;
    }

    sentry_path_t *dir = session_replay_dir(options);
    if (!dir) {
        return;
    }

    sentry_path_t *sidecar_path = replay_file_path(dir, replay_id, ".json");
    sentry_path_t *mp4_path = replay_file_path(dir, replay_id, ".mp4");
    sentry__path_free(dir);
    if (!sidecar_path || !mp4_path) {
        sentry__path_free(sidecar_path);
        sentry__path_free(mp4_path);
        return;
    }

    size_t json_len = 0;
    char *json = sentry__path_read_to_buffer(sidecar_path, &json_len);
    if (json) {
        sentry_value_t meta = sentry__value_from_json(json, json_len);
        sentry_free(json);

        // build_replay_envelope falls back to the sidecar's end timestamp if 0.
        double end_sec = 0.0;
        const char *ts = sentry_value_as_string(
            sentry_value_get_by_key(scope_source, "timestamp"));
        if (ts && ts[0]) {
            uint64_t usec = sentry__iso8601_to_usec(ts);
            if (usec) {
                end_sec = (double)usec / 1000000.0;
            }
        }

        sentry_envelope_t *envelope = build_replay_envelope(
            options, meta, mp4_path, end_sec, scope_source);
        if (envelope) {
            sentry__capture_envelope(transport, envelope, options);
        }
        sentry_value_decref(meta);

        sentry__path_remove(mp4_path);
        sentry__path_remove(sidecar_path);
    }

    sentry__path_free(mp4_path);
    sentry__path_free(sidecar_path);
}
