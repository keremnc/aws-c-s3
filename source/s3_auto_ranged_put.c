
#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3_meta_request_impl.h"
#include "aws/s3/private/s3_request_messages.h"
#include "aws/s3/private/s3_util.h"
#include <aws/common/string.h>
#include <aws/io/stream.h>

enum aws_s3_auto_ranged_put_state {
    AWS_S3_AUTO_RANGED_PUT_STATE_START,
    AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_CREATE,
    AWS_S3_AUTO_RANGED_PUT_STATE_SENDING_PARTS,
    AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_PARTS,
    AWS_S3_AUTO_RANGED_PUT_STATE_SEND_COMPLETE,
    AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_COMPLETE,
    AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_SINGLE_REQUEST
};

enum aws_s3_auto_ranged_put_request_tag {
    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_ENTIRE_OBJECT,
    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD,
    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_PART,
    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD
};

struct aws_s3_auto_ranged_put {
    struct aws_s3_meta_request base;

    struct {
        enum aws_s3_auto_ranged_put_state state;
        struct aws_array_list etag_list;

        uint32_t next_part_number;
        uint32_t total_num_parts;
        uint32_t num_parts_completed;
        uint64_t total_object_size;

        struct aws_string *upload_id;

    } synced_data;
};

static const size_t s_etags_initial_capacity = 16;
static const uint32_t g_max_num_upload_parts = 10000;

static void s_s3_auto_ranged_put_lock_synced_data(struct aws_s3_auto_ranged_put *auto_ranged_put);
static void s_s3_auto_ranged_put_unlock_synced_data(struct aws_s3_auto_ranged_put *auto_ranged_put);

static void s_s3_meta_request_auto_ranged_put_destroy(struct aws_s3_meta_request *meta_request);

static bool s_s3_auto_ranged_put_state_has_work(enum aws_s3_auto_ranged_put_state state);
static bool s_s3_auto_ranged_put_has_work(const struct aws_s3_meta_request *meta_request);

static int s_s3_auto_ranged_put_next_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request_desc **out_request_desc);

static struct aws_s3_request *s_s3_auto_ranged_put_request_factory(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_client *client,
    struct aws_s3_request_desc *request_desc);

static int s_s3_auto_ranged_put_incoming_headers(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *headers,
    size_t headers_count,
    void *user_data);

static int s_s3_auto_ranged_put_incoming_body(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    void *user_data);

static void s_s3_auto_ranged_put_stream_complete(struct aws_http_stream *stream, int error_code, void *user_data);

static struct aws_s3_meta_request_vtable s_s3_auto_ranged_put_vtable = {
    .has_work = s_s3_auto_ranged_put_has_work,
    .next_request = s_s3_auto_ranged_put_next_request,
    .request_factory = s_s3_auto_ranged_put_request_factory,
    .incoming_headers = s_s3_auto_ranged_put_incoming_headers,
    .incoming_headers_block_done = NULL,
    .incoming_body = s_s3_auto_ranged_put_incoming_body,
    .stream_complete = s_s3_auto_ranged_put_stream_complete,
    .destroy = s_s3_meta_request_auto_ranged_put_destroy};

static void s_s3_auto_ranged_put_lock_synced_data(struct aws_s3_auto_ranged_put *auto_ranged_put) {
    AWS_PRECONDITION(auto_ranged_put);

    aws_mutex_lock(&auto_ranged_put->base.synced_data.lock);
}

static void s_s3_auto_ranged_put_unlock_synced_data(struct aws_s3_auto_ranged_put *auto_ranged_put) {
    AWS_PRECONDITION(auto_ranged_put);

    aws_mutex_unlock(&auto_ranged_put->base.synced_data.lock);
}

/* Allocate a new auto-ranged put meta request */
struct aws_s3_meta_request *aws_s3_meta_request_auto_ranged_put_new(
    struct aws_allocator *allocator,
    const struct aws_s3_meta_request_internal_options *options) {

    AWS_PRECONDITION(options);

    /* These should already have been validated by the caller. */
    const struct aws_s3_meta_request_options *meta_request_options = options->options;
    AWS_PRECONDITION(meta_request_options);
    AWS_PRECONDITION(meta_request_options->message);

    /* We are not guaranteed a body stream from the user at this point, so make sure that we have one. */
    if (aws_http_message_get_body_stream(meta_request_options->message) == NULL) {
        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "Could not create auto-ranged-put meta request; body stream is NULL.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_s3_auto_ranged_put *auto_ranged_put =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_auto_ranged_put));

    if (aws_s3_meta_request_init_base(
            allocator, options, auto_ranged_put, &s_s3_auto_ranged_put_vtable, &auto_ranged_put->base)) {
        goto error_clean_up;
    }

    if (aws_array_list_init_dynamic(
            &auto_ranged_put->synced_data.etag_list,
            allocator,
            s_etags_initial_capacity,
            sizeof(struct aws_string *))) {
        goto error_clean_up;
    }

    return &auto_ranged_put->base;

error_clean_up:

    aws_mem_release(allocator, auto_ranged_put);
    return NULL;
}

/* Destroy our auto-ranged put meta request */
static void s_s3_meta_request_auto_ranged_put_destroy(struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;

    if (auto_ranged_put->synced_data.upload_id != NULL) {
        aws_string_destroy(auto_ranged_put->synced_data.upload_id);
        auto_ranged_put->synced_data.upload_id = NULL;
    }

    for (size_t etag_index = 0; etag_index < aws_array_list_length(&auto_ranged_put->synced_data.etag_list);
         ++etag_index) {
        struct aws_string *etag = NULL;

        aws_array_list_get_at(&auto_ranged_put->synced_data.etag_list, &etag, etag_index);

        if (etag != NULL) {
            aws_string_destroy(etag);
            etag = NULL;
        }
    }

    aws_array_list_clean_up(&auto_ranged_put->synced_data.etag_list);

    aws_mem_release(meta_request->allocator, auto_ranged_put);
}

static bool s_s3_auto_ranged_put_state_has_work(enum aws_s3_auto_ranged_put_state state) {
    return state == AWS_S3_AUTO_RANGED_PUT_STATE_START || state == AWS_S3_AUTO_RANGED_PUT_STATE_SENDING_PARTS ||
           state == AWS_S3_AUTO_RANGED_PUT_STATE_SEND_COMPLETE;
}

static bool s_s3_auto_ranged_put_has_work(const struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;

    bool has_work = false;

    s_s3_auto_ranged_put_lock_synced_data(auto_ranged_put);
    has_work = s_s3_auto_ranged_put_state_has_work(auto_ranged_put->synced_data.state);
    s_s3_auto_ranged_put_unlock_synced_data(auto_ranged_put);

    return has_work;
}

static int s_s3_auto_ranged_put_next_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request_desc **out_request_desc) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(out_request_desc);

    struct aws_s3_request_desc *request_desc = NULL;
    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;

    int result = AWS_OP_SUCCESS;

    s_s3_auto_ranged_put_lock_synced_data(auto_ranged_put);

    switch (auto_ranged_put->synced_data.state) {
        case AWS_S3_AUTO_RANGED_PUT_STATE_START: {

            struct aws_input_stream *initial_request_body = meta_request->synced_data.initial_body_stream;

            AWS_FATAL_ASSERT(initial_request_body);

            int64_t request_body_length = 0;

            /* TODO To support infinite streams, maybe let the user define the length themselves. */
            if (aws_input_stream_get_length(initial_request_body, &request_body_length)) {
                result = AWS_OP_ERR;
                goto unlock;
            }

            /* TODO add additional error checking for this going out of bounds. */
            auto_ranged_put->synced_data.total_num_parts = (uint32_t)(request_body_length / meta_request->part_size);

            if (request_body_length % meta_request->part_size) {
                ++auto_ranged_put->synced_data.total_num_parts;
            }

            /* TODO add better support for this. */
            AWS_ASSERT(auto_ranged_put->synced_data.total_num_parts < g_max_num_upload_parts);
            (void)g_max_num_upload_parts;

            /* If we're less than a part size, don't bother with a multipart upload. */
            if (request_body_length <= (int64_t)meta_request->part_size) {
                request_desc =
                    aws_s3_request_desc_new(meta_request, AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_ENTIRE_OBJECT, 0);

                /* Wait for this request to be processed before quitting. */
                auto_ranged_put->synced_data.state = AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_SINGLE_REQUEST;
            } else {

                /* Setup for a create-multipart upload */
                request_desc = aws_s3_request_desc_new(
                    meta_request, AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD, 0);

                /* We'll need to wait for the initial create to get back so that we can get the upload-id. */
                auto_ranged_put->synced_data.state = AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_CREATE;

                auto_ranged_put->synced_data.next_part_number = 1;
            }

            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_CREATE: {
            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_STATE_SENDING_PARTS: {

            /* Keep setting up to send parts until we've sent all of them at least once. */
            if (auto_ranged_put->synced_data.next_part_number <= auto_ranged_put->synced_data.total_num_parts) {
                request_desc = aws_s3_request_desc_new(
                    meta_request,
                    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_PART,
                    auto_ranged_put->synced_data.next_part_number);

                ++auto_ranged_put->synced_data.next_part_number;
            } else {
                auto_ranged_put->synced_data.state = AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_PARTS;
            }

            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_PARTS: {
            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_STATE_SEND_COMPLETE: {
            /* If all parts have been completed, set up to send a complete-multipart-upload request. */
            request_desc =
                aws_s3_request_desc_new(meta_request, AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD, 0);

            auto_ranged_put->synced_data.state = AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_COMPLETE;
            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_COMPLETE: {
            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_SINGLE_REQUEST: {
            break;
        }
        default:
            AWS_FATAL_ASSERT(false);
            break;
    }

unlock:

    s_s3_auto_ranged_put_unlock_synced_data(auto_ranged_put);

    if (request_desc != NULL) {
        AWS_LOGF_TRACE(
            AWS_LS_S3_META_REQUEST,
            "id=%p: Returning request desc for part %d of %d",
            (void *)meta_request,
            request_desc->part_number,
            auto_ranged_put->synced_data.total_num_parts);
    }

    *out_request_desc = request_desc;

    return result;
}

/* Create an in flight request given a request description. */
static struct aws_s3_request *s_s3_auto_ranged_put_request_factory(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_client *client,
    struct aws_s3_request_desc *request_desc) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(request_desc);

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;
    AWS_PRECONDITION(auto_ranged_put);

    struct aws_http_message *message = NULL;
    struct aws_s3_part_buffer *part_buffer = NULL;

    /* If we're not sending the entire object, then we need to grab a part buffer */
    if (request_desc->request_tag != AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_ENTIRE_OBJECT) {

        part_buffer = aws_s3_client_get_part_buffer(client, request_desc->part_number);

        if (part_buffer == NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_META_REQUEST,
                "id=%p Could not get part buffer for request with tag %d for auto-ranged-get meta request.",
                (void *)meta_request,
                request_desc->request_tag);

            aws_raise_error(AWS_ERROR_S3_NO_PART_BUFFER);
            goto part_buffer_get_failed;
        }
    }

    switch (request_desc->request_tag) {

        /* If we're grabbing the whole object, just use the original message. */
        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_ENTIRE_OBJECT: {
            message = meta_request->initial_request_message;

            aws_http_message_acquire(message);

            AWS_FATAL_ASSERT(message);
            break;
        }

        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_PART: {

            bool error_occurred = false;

            s_s3_auto_ranged_put_lock_synced_data(auto_ranged_put);

            uint64_t range_start = (request_desc->part_number - 1) * meta_request->part_size;
            struct aws_input_stream *initial_body_stream = meta_request->synced_data.initial_body_stream;

            AWS_FATAL_ASSERT(initial_body_stream);

            struct aws_byte_cursor buffer_byte_cursor = aws_byte_cursor_from_buf(&part_buffer->buffer);
            AWS_FATAL_ASSERT(buffer_byte_cursor.len <= meta_request->part_size);

            /* Seek to our part of the original input stream. */
            /* TODO don't seek, read the file in order. */
            if (aws_input_stream_seek(initial_body_stream, range_start, AWS_SSB_BEGIN)) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Could not seek initial body stream for request with tag %d for auto-ranged-put meta "
                    "request.",
                    (void *)meta_request,
                    request_desc->request_tag);
                error_occurred = true;
                goto unlock;
            }

            /* Copy it into our part buffer. */
            if (aws_input_stream_read(initial_body_stream, &part_buffer->buffer)) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Could not read from initial body stream for request with tag %d for auto-ranged-put "
                    "meta "
                    "request.",
                    (void *)meta_request,
                    request_desc->request_tag);
                error_occurred = true;
                goto unlock;
            }

        unlock:
            s_s3_auto_ranged_put_unlock_synced_data(auto_ranged_put);

            if (error_occurred) {
                goto message_create_failed;
            }

            /* Create a new put-object message to upload a part. */
            message = aws_s3_put_object_message_new(
                meta_request->allocator,
                meta_request->initial_request_message,
                &part_buffer->buffer,
                request_desc->part_number,
                auto_ranged_put->synced_data.upload_id);

            if (message == NULL) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Could not allocate message for request with tag %d for auto-ranged-put meta request.",
                    (void *)meta_request,
                    request_desc->request_tag);
                goto message_create_failed;
            }

        } break;
        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD: {

            /* Create the message to create a new multipart upload. */
            message = aws_s3_create_multipart_upload_message_new(
                meta_request->allocator, meta_request->initial_request_message);

            if (message == NULL) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Could not allocate message for request with tag %d for auto-ranged-put meta request.",
                    (void *)meta_request,
                    request_desc->request_tag);
                goto message_create_failed;
            }

            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD: {
            s_s3_auto_ranged_put_lock_synced_data(auto_ranged_put);

            AWS_FATAL_ASSERT(auto_ranged_put->synced_data.upload_id);

            /* Build the message to complete our multipart upload, which includes a payload describing all of our
             * completed parts. */
            message = aws_s3_complete_multipart_message_new(
                meta_request->allocator,
                meta_request->initial_request_message,
                &part_buffer->buffer,
                auto_ranged_put->synced_data.upload_id,
                &auto_ranged_put->synced_data.etag_list);

            s_s3_auto_ranged_put_unlock_synced_data(auto_ranged_put);

            if (message == NULL) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Could not allocate message for request with tag %d for auto-ranged-put meta request.",
                    (void *)meta_request,
                    request_desc->request_tag);
                goto message_create_failed;
            }

            break;
        }
    }

    /* Allocate the actual in-flight request structure. */
    struct aws_s3_request *request = aws_s3_request_new(meta_request, message);
    aws_http_message_release(message);

    if (request == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p Could not not allocate request with tag %d for Auto-Ranged-Get Meta Request.",
            (void *)meta_request,
            request_desc->request_tag);

        if (message != NULL) {
            aws_http_message_release(message);
            message = NULL;
        }

        if (part_buffer != NULL) {
            aws_s3_part_buffer_release(part_buffer);
            part_buffer = NULL;
        }

        return NULL;
    }

    AWS_LOGF_TRACE(
        AWS_LS_S3_META_REQUEST,
        "id=%p: Created request %p for part %d",
        (void *)meta_request,
        (void *)request,
        request_desc->part_number);

    request->part_buffer = part_buffer;

    return request;

message_create_failed:

    return NULL;

part_buffer_get_failed:

    return NULL;
}

static int s_s3_auto_ranged_put_incoming_headers(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *headers,
    size_t headers_count,
    void *user_data) {

    (void)stream;
    (void)header_block;

    AWS_PRECONDITION(stream);

    struct aws_s3_send_request_work *work = user_data;
    AWS_PRECONDITION(work);

    struct aws_s3_request_desc *request_desc = work->request_desc;
    AWS_PRECONDITION(request_desc);

    if (request_desc->request_tag != AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_PART) {
        return AWS_OP_SUCCESS;
    }

    struct aws_s3_meta_request *meta_request = work->meta_request;
    AWS_PRECONDITION(meta_request);

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;
    AWS_PRECONDITION(auto_ranged_put);

    struct aws_allocator *allocator = meta_request->allocator;
    AWS_PRECONDITION(allocator);

    size_t part_number = work->request_desc->part_number;
    AWS_FATAL_ASSERT(part_number > 0);
    size_t part_index = part_number - 1;

    int result = AWS_OP_SUCCESS;

    /* Find the ETag header if it exists and cache it. */
    for (size_t i = 0; i < headers_count; ++i) {
        const struct aws_byte_cursor *name = &headers[i].name;
        const struct aws_byte_cursor *value = &headers[i].value;

        if (!aws_http_header_name_eq(*name, g_etag_header_name)) {
            continue;
        }

        struct aws_byte_cursor value_within_quotes = *value;

        /* The ETag value arrives in quotes, but we don't want it in quotes when we send it back up later, so just
         * get rid of the quotes now. */
        if (value_within_quotes.len >= 2 && value_within_quotes.ptr[0] == '"' &&
            value_within_quotes.ptr[value_within_quotes.len - 1] == '"') {

            aws_byte_cursor_advance(&value_within_quotes, 1);
            --value_within_quotes.len;
        }

        struct aws_string *etag = aws_string_new_from_cursor(allocator, &value_within_quotes);
        struct aws_string *null_etag = NULL;

        s_s3_auto_ranged_put_lock_synced_data(auto_ranged_put);

        /* ETags need to be associated with their part number, so we keep the etag indices consistent with part
         * numbers. This means we may have to add padding to the list in the case that parts finish out of order. */
        while (aws_array_list_length(&auto_ranged_put->synced_data.etag_list) < part_number) {
            if (aws_array_list_push_back(&auto_ranged_put->synced_data.etag_list, &null_etag)) {
                result = AWS_OP_ERR;
                goto unlock;
            }
        }

        aws_array_list_set_at(&auto_ranged_put->synced_data.etag_list, &etag, part_index);
    unlock:
        s_s3_auto_ranged_put_unlock_synced_data(auto_ranged_put);

        break;
    }

    return result;
}

static int s_s3_auto_ranged_put_incoming_body(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    void *user_data) {

    AWS_PRECONDITION(stream);
    (void)stream;

    struct aws_s3_send_request_work *work = user_data;
    AWS_PRECONDITION(work);
    AWS_PRECONDITION(work->meta_request);

    struct aws_s3_request *request = work->request;
    AWS_PRECONDITION(request);

    struct aws_s3_request_desc *request_desc = work->request_desc;
    AWS_PRECONDITION(request_desc);

    struct aws_s3_part_buffer *part_buffer = request->part_buffer;
    AWS_PRECONDITION(part_buffer);

    if (request_desc->request_tag == AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD) {
        if (aws_byte_buf_append(&part_buffer->buffer, data)) {
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

static void s_s3_auto_ranged_put_stream_complete(struct aws_http_stream *stream, int error_code, void *user_data) {
    AWS_PRECONDITION(stream);
    (void)stream;

    struct aws_s3_send_request_work *work = user_data;
    AWS_PRECONDITION(work);
    AWS_PRECONDITION(work->request_desc);

    struct aws_s3_meta_request *meta_request = work->meta_request;
    AWS_PRECONDITION(meta_request);

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;
    AWS_PRECONDITION(auto_ranged_put);

    if (error_code != AWS_ERROR_SUCCESS) {

        /* Retry if the error was service side or we just ran out of parts.  Otherwise, fail the meta request. */
        /* TODO try to guarantee part buffers ara available earlier. */
        if (error_code == AWS_ERROR_S3_INTERNAL_ERROR || error_code == AWS_ERROR_S3_NO_PART_BUFFER) {
            aws_s3_meta_request_queue_retry(meta_request, &work->request_desc);
        } else {
            aws_s3_meta_request_finish(meta_request, error_code);
        }

        return;
    }

    struct aws_s3_request *request = work->request;
    AWS_PRECONDITION(request);

    struct aws_s3_request_desc *request_desc = work->request_desc;
    AWS_PRECONDITION(request_desc);

    struct aws_s3_part_buffer *part_buffer = request->part_buffer;
    AWS_PRECONDITION(part_buffer);

    if (request_desc->request_tag == AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD) {

        struct aws_byte_cursor buffer_byte_cursor = aws_byte_cursor_from_buf(&part_buffer->buffer);

        /* Find the upload id for this multipart upload. */
        struct aws_string *upload_id =
            aws_s3_create_multipart_upload_get_upload_id(meta_request->allocator, &buffer_byte_cursor);

        if (upload_id == NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_META_REQUEST,
                "id=%p Could not find upload-id in create-multipart-upload response",
                (void *)meta_request);

            aws_s3_meta_request_finish(meta_request, AWS_ERROR_S3_MISSING_UPLOAD_ID);
            return;
        }

        s_s3_auto_ranged_put_lock_synced_data(auto_ranged_put);

        /* Store the multipart upload id and set that we are ready for sending parts. */
        auto_ranged_put->synced_data.upload_id = upload_id;
        auto_ranged_put->synced_data.state = AWS_S3_AUTO_RANGED_PUT_STATE_SENDING_PARTS;

        s_s3_auto_ranged_put_unlock_synced_data(auto_ranged_put);

        /* Create Multipart Upload finished successfully, so now we should have parts to send. */
        aws_s3_client_schedule_meta_request_work(meta_request->client, &auto_ranged_put->base);

    } else if (request_desc->request_tag == AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_PART) {

        bool notify_work_available = false;

        s_s3_auto_ranged_put_lock_synced_data(auto_ranged_put);
        ++auto_ranged_put->synced_data.num_parts_completed;

        if (auto_ranged_put->synced_data.num_parts_completed == auto_ranged_put->synced_data.total_num_parts) {
            auto_ranged_put->synced_data.state = AWS_S3_AUTO_RANGED_PUT_STATE_SEND_COMPLETE;
            notify_work_available = true;
        }

        AWS_LOGF_DEBUG(
            AWS_LS_S3_META_REQUEST,
            "id=%p: %d out of %d parts have completed.",
            (void *)meta_request,
            auto_ranged_put->synced_data.num_parts_completed,
            auto_ranged_put->synced_data.total_num_parts);

        s_s3_auto_ranged_put_unlock_synced_data(auto_ranged_put);

        if (notify_work_available) {
            aws_s3_client_schedule_meta_request_work(meta_request->client, &auto_ranged_put->base);
        }

    } else if (
        request_desc->request_tag == AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD ||
        request_desc->request_tag == AWS_S3_AUTO_RANGED_PUT_STATE_WAITING_FOR_SINGLE_REQUEST) {
        aws_s3_meta_request_finish(meta_request, AWS_ERROR_SUCCESS);
    } else {
        AWS_FATAL_ASSERT(false);
    }
}