/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_auto_ranged_put.h"
#include "aws/s3/private/s3_checksums.h"
#include "aws/s3/private/s3_list_parts.h"
#include "aws/s3/private/s3_request_messages.h"
#include "aws/s3/private/s3_util.h"
#include <aws/common/json.h>
#include <aws/common/string.h>
#include <aws/io/stream.h>

static const struct aws_byte_cursor s_upload_id = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("UploadId");
static const size_t s_complete_multipart_upload_init_body_size_bytes = 512;
static const size_t s_abort_multipart_upload_init_body_size_bytes = 512;

static const struct aws_byte_cursor s_create_multipart_upload_copy_headers[] = {
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-algorithm"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-key-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-context"),
};

static void s_s3_meta_request_auto_ranged_put_destroy(struct aws_s3_meta_request *meta_request);

static bool s_s3_auto_ranged_put_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request);

static int s_s3_auto_ranged_put_prepare_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request);

static void s_s3_auto_ranged_put_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code);

static int s_s3_auto_ranged_put_pause(struct aws_s3_meta_request *meta_request, struct aws_string **resume_token);

static bool s_process_part_info(const struct aws_s3_part_info *info, void *user_data) {
    struct aws_s3_auto_ranged_put *auto_ranged_put = user_data;

    struct aws_string *etag = aws_strip_quotes(auto_ranged_put->base.allocator, info->e_tag);

    const struct aws_byte_cursor *checksum_cur = NULL;
    switch (auto_ranged_put->base.checksum_config.checksum_algorithm) {
        case AWS_SCA_CRC32:
            checksum_cur = &info->checksumCRC32;
            break;
        case AWS_SCA_CRC32C:
            checksum_cur = &info->checksumCRC32C;
            break;
        case AWS_SCA_SHA1:
            checksum_cur = &info->checksumSHA1;
            break;
        case AWS_SCA_SHA256:
            checksum_cur = &info->checksumSHA256;
            break;
        case AWS_SCA_NONE:
            break;
        default:
            AWS_ASSERT(false);
            break;
    }

    if (checksum_cur) {
        aws_byte_buf_init_copy_from_cursor(
            &auto_ranged_put->checksums_list[info->part_number - 1], auto_ranged_put->base.allocator, *checksum_cur);
    }

    aws_array_list_set_at(&auto_ranged_put->synced_data.etag_list, &etag, info->part_number - 1);

    return true;
}

static int s_parse_resume_info_from_token(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *resume_token,
    struct aws_string **upload_id_out,
    size_t *part_size_out,
    uint32_t *total_num_parts_out) {

    if (!resume_token) {
        return AWS_OP_SUCCESS;
    }

    struct aws_json_value *root = aws_json_value_new_from_string(allocator, *resume_token);

    struct aws_json_value *type_node = aws_json_value_get_from_object(root, aws_byte_cursor_from_c_str("type"));

    struct aws_json_value *multipart_upload_id_node =
        aws_json_value_get_from_object(root, aws_byte_cursor_from_c_str("multipart_upload_id"));

    struct aws_json_value *partition_size_node =
        aws_json_value_get_from_object(root, aws_byte_cursor_from_c_str("partition_size"));

    struct aws_json_value *total_num_parts_node =
        aws_json_value_get_from_object(root, aws_byte_cursor_from_c_str("total_num_parts"));

    double partition_size_value = 0.0;
    double total_num_parts_value = 0.0;
    struct aws_byte_cursor multipart_upload_id_value;
    struct aws_byte_cursor type_value;

    if (!multipart_upload_id_node || !partition_size_node || !total_num_parts_node || !type_node ||
        aws_json_value_get_number(partition_size_node, &partition_size_value) ||
        aws_json_value_get_number(total_num_parts_node, &total_num_parts_value) ||
        aws_json_value_get_string(multipart_upload_id_node, &multipart_upload_id_value) ||
        aws_json_value_get_string(type_node, &type_value)) {
        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "Could not load persisted state. Invalid token.");
        goto invalid_argument_cleanup;
    }

    if (!aws_byte_cursor_eq_c_str(&type_value, "AWS_S3_META_REQUEST_TYPE_PUT_OBJECT")) {
        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "Could not load persisted state. Invalid token type.");
        goto invalid_argument_cleanup;
    }

    if ((size_t)partition_size_value < g_s3_min_upload_part_size) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create resume auto-ranged-put meta request; part size of %" PRIu64
            " specified in the token is below minimum threshold for multi-part.",
            (uint64_t)partition_size_value);

        goto invalid_argument_cleanup;
    }

    if ((uint32_t)total_num_parts_value > g_s3_max_num_upload_parts) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create resume auto-ranged-put meta request; total number of parts %" PRIu32
            " specified in the token is too large for platform.",
            (uint32_t)total_num_parts_value);

        goto invalid_argument_cleanup;
    }

    *upload_id_out = aws_string_new_from_cursor(allocator, &multipart_upload_id_value);
    *part_size_out = (size_t)partition_size_value;
    *total_num_parts_out = (uint32_t)total_num_parts_value;

    aws_json_value_destroy(root);

    return AWS_OP_SUCCESS;

invalid_argument_cleanup:
    aws_json_value_destroy(root);
    return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
}

/**
 * Loads the persistable state used to resume an upload that was previously paused.
 */
static int s_load_persistable_state(
    struct aws_allocator *allocator,
    struct aws_s3_auto_ranged_put *auto_ranged_put,
    uint64_t content_length,
    struct aws_string *upload_id,
    size_t part_size,
    uint32_t total_num_parts) {

    uint32_t num_parts = (uint32_t)(content_length / part_size);

    if ((content_length % part_size) > 0) {
        ++num_parts;
    }

    if (total_num_parts != num_parts) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create auto-ranged-put meta request; persisted number of parts %" PRIu32
            " does not match expected number of parts based on length of the body.",
            total_num_parts);

        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_byte_cursor request_path;
    if (aws_http_message_get_request_path(auto_ranged_put->base.initial_request_message, &request_path)) {
        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "Could not load persisted state. Request path could not be read.");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    auto_ranged_put->synced_data.num_parts_sent = 0;
    auto_ranged_put->synced_data.num_parts_completed = 0;
    auto_ranged_put->synced_data.create_multipart_upload_sent = true;
    auto_ranged_put->synced_data.create_multipart_upload_completed = true;
    auto_ranged_put->upload_id = upload_id;

    struct aws_s3_list_parts_params list_parts_params = {
        .key = request_path,
        .upload_id = aws_byte_cursor_from_string(auto_ranged_put->upload_id),
        .on_part = s_process_part_info,
        .user_data = auto_ranged_put,
    };

    auto_ranged_put->synced_data.list_parts_operation = aws_s3_list_parts_operation_new(allocator, &list_parts_params);

    struct aws_http_headers *needed_response_headers = aws_http_headers_new(allocator);
    const size_t copy_header_count = sizeof(s_create_multipart_upload_copy_headers) / sizeof(struct aws_byte_cursor);
    struct aws_http_headers *initial_headers =
        aws_http_message_get_headers(auto_ranged_put->base.initial_request_message);

    /* Copy headers that would have been used for create multi part from initial message, since create will never be
     * called in this flow */
    for (size_t header_index = 0; header_index < copy_header_count; ++header_index) {
        const struct aws_byte_cursor *header_name = &s_create_multipart_upload_copy_headers[header_index];
        struct aws_byte_cursor header_value;
        AWS_ZERO_STRUCT(header_value);

        if (!aws_http_headers_get(initial_headers, *header_name, &header_value)) {
            aws_http_headers_set(needed_response_headers, *header_name, header_value);
        }
    }

    auto_ranged_put->synced_data.needed_response_headers = needed_response_headers;

    return AWS_OP_SUCCESS;
}

static struct aws_s3_meta_request_vtable s_s3_auto_ranged_put_vtable = {
    .update = s_s3_auto_ranged_put_update,
    .send_request_finish = aws_s3_meta_request_send_request_finish_default,
    .prepare_request = s_s3_auto_ranged_put_prepare_request,
    .init_signing_date_time = aws_s3_meta_request_init_signing_date_time_default,
    .sign_request = aws_s3_meta_request_sign_request_default,
    .finished_request = s_s3_auto_ranged_put_request_finished,
    .destroy = s_s3_meta_request_auto_ranged_put_destroy,
    .finish = aws_s3_meta_request_finish_default,
    .pause = s_s3_auto_ranged_put_pause,
};

/* Allocate a new auto-ranged put meta request */
struct aws_s3_meta_request *aws_s3_meta_request_auto_ranged_put_new(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    size_t part_size,
    uint64_t content_length,
    uint32_t num_parts,
    const struct aws_s3_meta_request_options *options) {

    /* These should already have been validated by the caller. */
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(options);
    AWS_PRECONDITION(options->message);
    AWS_PRECONDITION(aws_http_message_get_body_stream(options->message));

    struct aws_s3_auto_ranged_put *auto_ranged_put =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_auto_ranged_put));

    struct aws_string *multipart_upload_id = NULL;
    if (s_parse_resume_info_from_token(
            allocator, options->resume_token, &multipart_upload_id, &part_size, &num_parts)) {
        goto error_clean_up;
    }

    if (aws_s3_meta_request_init_base(
            allocator,
            client,
            part_size,
            client->compute_content_md5 == AWS_MR_CONTENT_MD5_ENABLED ||
                aws_http_headers_has(aws_http_message_get_headers(options->message), g_content_md5_header_name),
            options,
            auto_ranged_put,
            &s_s3_auto_ranged_put_vtable,
            &auto_ranged_put->base)) {
        goto error_clean_up;
    }

    auto_ranged_put->content_length = content_length;
    auto_ranged_put->synced_data.total_num_parts = num_parts;
    auto_ranged_put->upload_id = NULL;

    auto_ranged_put->threaded_update_data.next_part_number = 1;
    auto_ranged_put->prepare_data.num_parts_read_from_stream = 0;

    if (options->resume_token) {
        if (s_load_persistable_state(
                allocator, auto_ranged_put, content_length, multipart_upload_id, part_size, num_parts)) {
            goto error_clean_up;
        }
    } else {
        auto_ranged_put->synced_data.list_parts_operation = NULL;
        auto_ranged_put->synced_data.list_parts_sent = true;
        auto_ranged_put->synced_data.list_parts_completed = true;
    }

    struct aws_string **etag_c_array = aws_mem_calloc(allocator, sizeof(struct aws_string *), num_parts);
    aws_array_list_init_static(
        &auto_ranged_put->synced_data.etag_list, etag_c_array, num_parts, sizeof(struct aws_string *));
    auto_ranged_put->checksums_list = aws_mem_calloc(allocator, sizeof(struct aws_byte_buf), num_parts);

    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST, "id=%p Created new Auto-Ranged Put Meta Request.", (void *)&auto_ranged_put->base);

    return &auto_ranged_put->base;

error_clean_up:
    aws_string_destroy(multipart_upload_id);
    aws_mem_release(allocator, auto_ranged_put);

    return NULL;
}

/* Destroy our auto-ranged put meta request */
static void s_s3_meta_request_auto_ranged_put_destroy(struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;

    aws_string_destroy(auto_ranged_put->upload_id);
    auto_ranged_put->upload_id = NULL;

    aws_s3_paginated_operation_release(auto_ranged_put->synced_data.list_parts_operation);

    for (size_t etag_index = 0; etag_index < aws_array_list_length(&auto_ranged_put->synced_data.etag_list);
         ++etag_index) {
        struct aws_string *etag = NULL;

        aws_array_list_get_at(&auto_ranged_put->synced_data.etag_list, &etag, etag_index);
        aws_string_destroy(etag);
    }
    aws_string_destroy(auto_ranged_put->synced_data.list_parts_continuation_token);
    /* probably just better to store num parts in the meta-request */
    for (size_t checksum_index = 0; checksum_index < aws_array_list_length(&auto_ranged_put->synced_data.etag_list);
         ++checksum_index) {
        aws_byte_buf_clean_up(&auto_ranged_put->checksums_list[checksum_index]);
    }
    aws_mem_release(meta_request->allocator, auto_ranged_put->synced_data.etag_list.data);
    aws_mem_release(meta_request->allocator, auto_ranged_put->checksums_list);
    aws_array_list_clean_up(&auto_ranged_put->synced_data.etag_list);
    aws_http_headers_release(auto_ranged_put->synced_data.needed_response_headers);
    aws_mem_release(meta_request->allocator, auto_ranged_put);
}

static bool s_s3_auto_ranged_put_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(out_request);

    struct aws_s3_request *request = NULL;
    bool work_remaining = false;

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_meta_request_lock_synced_data(meta_request);

        if (!aws_s3_meta_request_has_finish_result_synced(meta_request)) {
            /* If resuming and list part has not be sent, do it now. */
            if (!auto_ranged_put->synced_data.list_parts_sent) {
                request = aws_s3_request_new(
                    meta_request,
                    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_LIST_PARTS,
                    0,
                    AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

                auto_ranged_put->synced_data.list_parts_sent = true;

                goto has_work_remaining;
            }

            /* waiting on list parts to finish. */
            if (!auto_ranged_put->synced_data.list_parts_completed) {
                goto has_work_remaining;
            }

            /* If we haven't already sent a create-multipart-upload message, do so now. */
            if (!auto_ranged_put->synced_data.create_multipart_upload_sent) {
                request = aws_s3_request_new(
                    meta_request,
                    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD,
                    0,
                    AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

                auto_ranged_put->synced_data.create_multipart_upload_sent = true;

                goto has_work_remaining;
            }

            /* If the create-multipart-upload message hasn't been completed, then there is still additional work to do,
             * but it can't be done yet. */
            if (!auto_ranged_put->synced_data.create_multipart_upload_completed) {
                goto has_work_remaining;
            }

            /* If we haven't sent all of the parts yet, then set up to send a new part now. */
            if (auto_ranged_put->synced_data.num_parts_sent < auto_ranged_put->synced_data.total_num_parts) {

                /* Check if the etag/checksum list has the result already */
                int part_index = auto_ranged_put->threaded_update_data.next_part_number - 1;
                for (size_t etag_index = part_index;
                     etag_index < aws_array_list_length(&auto_ranged_put->synced_data.etag_list);
                     ++etag_index) {
                    struct aws_string *etag = NULL;

                    if (!aws_array_list_get_at(&auto_ranged_put->synced_data.etag_list, &etag, etag_index) && etag) {
                        /* part already downloaded, skip it here and prepare will take care of adjusting the buffer */
                        ++auto_ranged_put->threaded_update_data.next_part_number;

                    } else {
                        // incomplete part found. break out and create request for it.
                        break;
                    }
                }

                // Something went really wrong. we still have parts to send, but have etags for all parts
                AWS_FATAL_ASSERT(
                    auto_ranged_put->threaded_update_data.next_part_number <=
                    auto_ranged_put->synced_data.total_num_parts)

                if ((flags & AWS_S3_META_REQUEST_UPDATE_FLAG_CONSERVATIVE) != 0) {
                    uint32_t num_parts_in_flight =
                        (auto_ranged_put->synced_data.num_parts_sent -
                         auto_ranged_put->synced_data.num_parts_completed);

                    /* Because uploads must read from their streams serially, we try to limit the amount of in flight
                     * requests for a given multipart upload if we can. */
                    if (num_parts_in_flight > 0) {
                        goto has_work_remaining;
                    }
                }

                /* Allocate a request for another part. */
                request = aws_s3_request_new(
                    meta_request,
                    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_PART,
                    0,
                    AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

                request->part_number = auto_ranged_put->threaded_update_data.next_part_number;

                ++auto_ranged_put->threaded_update_data.next_part_number;
                ++auto_ranged_put->synced_data.num_parts_sent;

                AWS_LOGF_DEBUG(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p: Returning request %p for part %d",
                    (void *)meta_request,
                    (void *)request,
                    request->part_number);

                goto has_work_remaining;
            }

            /* There is one more request to send after all of the parts (the complete-multipart-upload) but it can't be
             * done until all of the parts have been completed.*/
            if (auto_ranged_put->synced_data.num_parts_completed != auto_ranged_put->synced_data.total_num_parts) {
                goto has_work_remaining;
            }

            /* If the complete-multipart-upload request hasn't been set yet, then send it now. */
            if (!auto_ranged_put->synced_data.complete_multipart_upload_sent) {
                request = aws_s3_request_new(
                    meta_request,
                    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD,
                    0,
                    AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

                auto_ranged_put->synced_data.complete_multipart_upload_sent = true;

                goto has_work_remaining;
            }

            /* Wait for the complete-multipart-upload request to finish. */
            if (!auto_ranged_put->synced_data.complete_multipart_upload_completed) {
                goto has_work_remaining;
            }

            goto no_work_remaining;
        } else {

            /* If the create multipart upload hasn't been sent, then there is nothing left to do when canceling. */
            if (!auto_ranged_put->synced_data.create_multipart_upload_sent) {
                goto no_work_remaining;
            }

            /* If the create-multipart-upload request is still in flight, wait for it to finish. */
            if (!auto_ranged_put->synced_data.create_multipart_upload_completed) {
                goto has_work_remaining;
            }

            /* If the number of parts completed is less than the number of parts sent, then we need to wait until all of
             * those parts are done sending before aborting. */
            if (auto_ranged_put->synced_data.num_parts_completed < auto_ranged_put->synced_data.num_parts_sent) {
                goto has_work_remaining;
            }

            /* If the complete-multipart-upload is already in flight, then we can't necessarily send an abort. */
            if (auto_ranged_put->synced_data.complete_multipart_upload_sent &&
                !auto_ranged_put->synced_data.complete_multipart_upload_completed) {
                goto has_work_remaining;
            }

            /* If the upload was paused or resume failed, we don't abort the multipart upload. */
            if (meta_request->synced_data.finish_result.error_code == AWS_ERROR_S3_PAUSED ||
                meta_request->synced_data.finish_result.error_code == AWS_ERROR_S3_RESUME_FAILED) {
                goto no_work_remaining;
            }

            /* If the complete-multipart-upload completed successfully, then there is nothing to abort since the
             * transfer has already finished. */
            if (auto_ranged_put->synced_data.complete_multipart_upload_completed &&
                auto_ranged_put->synced_data.complete_multipart_upload_error_code == AWS_ERROR_SUCCESS) {
                goto no_work_remaining;
            }

            /* If we made it here, and the abort-multipart-upload message hasn't been sent yet, then do so now. */
            if (!auto_ranged_put->synced_data.abort_multipart_upload_sent) {
                if (auto_ranged_put->upload_id == NULL) {
                    goto no_work_remaining;
                }

                request = aws_s3_request_new(
                    meta_request,
                    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_ABORT_MULTIPART_UPLOAD,
                    0,
                    AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS | AWS_S3_REQUEST_FLAG_ALWAYS_SEND);

                auto_ranged_put->synced_data.abort_multipart_upload_sent = true;

                goto has_work_remaining;
            }

            /* Wait for the multipart upload to be completed. */
            if (!auto_ranged_put->synced_data.abort_multipart_upload_completed) {
                goto has_work_remaining;
            }

            goto no_work_remaining;
        }

    has_work_remaining:
        work_remaining = true;

    no_work_remaining:

        if (!work_remaining) {
            aws_s3_meta_request_set_success_synced(meta_request, AWS_S3_RESPONSE_STATUS_SUCCESS);
        }

        aws_s3_meta_request_unlock_synced_data(meta_request);
    }
    /* END CRITICAL SECTION */

    if (work_remaining) {
        *out_request = request;
    } else {
        AWS_ASSERT(request == NULL);

        aws_s3_meta_request_finish(meta_request);
    }

    return work_remaining;
}

/**
 * Helper to compute request body size.
 * Basically returns either part size or if content is not equally divisible into parts, the size of the remaining last
 * part.
 */
static size_t s_compute_request_body_size(struct aws_s3_meta_request *meta_request, uint32_t part_number) {
    AWS_PRECONDITION(meta_request);

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;

    size_t request_body_size = meta_request->part_size;
    /* Last part--adjust size to match remaining content length. */
    if (part_number == auto_ranged_put->synced_data.total_num_parts) {
        size_t content_remainder = (size_t)(auto_ranged_put->content_length % (uint64_t)meta_request->part_size);

        if (content_remainder > 0) {
            request_body_size = content_remainder;
        }
    }

    return request_body_size;
}

/**
 * Skips parts from input stream that were previously uploaded.
 * Assumes input stream has num_parts_read_from_stream specifying which part stream is on
 * and will read into temp buffer until it gets to skip_until_part_number (i.e. skipping does include
 * that part). If checksum is set on the request and parts with checksums were uploaded before, checksum will be
 * verified.
 */
static int s_skip_parts_from_stream(
    struct aws_s3_meta_request *meta_request,
    uint32_t num_parts_read_from_stream,
    uint32_t skip_until_part_number) {

    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(num_parts_read_from_stream <= skip_until_part_number);

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;

    AWS_PRECONDITION(skip_until_part_number <= auto_ranged_put->synced_data.total_num_parts);

    if (num_parts_read_from_stream == skip_until_part_number) {
        return AWS_OP_SUCCESS;
    }

    struct aws_byte_buf temp_body_buf;
    aws_byte_buf_init(&temp_body_buf, meta_request->allocator, 0);

    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p: Skipping parts %d through %d",
        (void *)meta_request,
        num_parts_read_from_stream,
        skip_until_part_number);

    for (uint32_t part_index = num_parts_read_from_stream; part_index < skip_until_part_number; ++part_index) {

        size_t request_body_size = s_compute_request_body_size(meta_request, part_index + 1);

        if (temp_body_buf.capacity != request_body_size) {
            // reinit with correct size
            aws_byte_buf_clean_up(&temp_body_buf);
            aws_byte_buf_init(&temp_body_buf, meta_request->allocator, request_body_size);
        } else {
            // reuse buffer
            aws_byte_buf_reset(&temp_body_buf, false);
        }

        if (aws_s3_meta_request_read_body(meta_request, &temp_body_buf)) {
            AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "Failed to resume upload. Input steam cannot be read.");
            aws_byte_buf_clean_up(&temp_body_buf);
            return AWS_OP_ERR;
        }

        // compare skipped checksum to previously uploaded checksum
        if (meta_request->checksum_config.checksum_algorithm != AWS_SCA_NONE &&
            auto_ranged_put->checksums_list[part_index].len > 0) {
            struct aws_byte_buf checksum;
            aws_byte_buf_init(
                &checksum,
                meta_request->allocator,
                aws_get_digest_size_from_algorithm(meta_request->checksum_config.checksum_algorithm));
            struct aws_byte_cursor body_cur = aws_byte_cursor_from_buf(&temp_body_buf);

            if (aws_checksum_compute(
                    meta_request->allocator,
                    meta_request->checksum_config.checksum_algorithm,
                    &body_cur,
                    &checksum,
                    0)) {
                AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "Failed to resume upload. Unable to compute checksum.");
                aws_byte_buf_clean_up(&temp_body_buf);
                aws_byte_buf_clean_up(&checksum);
                return aws_raise_error(AWS_ERROR_S3_RESUME_FAILED);
            }

            if (!aws_byte_buf_eq(&checksum, &auto_ranged_put->checksums_list[part_index])) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "Failed to resume upload. Checksum for previously uploaded part does not match "
                    "new part.");
                aws_byte_buf_clean_up(&temp_body_buf);
                aws_byte_buf_clean_up(&checksum);
                return aws_raise_error(AWS_ERROR_S3_RESUMED_PART_CHECKSUM_MISMATCH);
            }
        }
    }

    aws_byte_buf_clean_up(&temp_body_buf);
    return AWS_OP_SUCCESS;
}

/* Given a request, prepare it for sending based on its description. */
static int s_s3_auto_ranged_put_prepare_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request) {
    AWS_PRECONDITION(meta_request);

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;
    AWS_PRECONDITION(auto_ranged_put);

    struct aws_http_message *message = NULL;

    switch (request->request_tag) {
        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_LIST_PARTS: {

            int message_creation_result = AWS_OP_ERR;
            /* BEGIN CRITICAL SECTION */
            {
                aws_s3_meta_request_lock_synced_data(meta_request);

                if (auto_ranged_put->synced_data.list_parts_continuation_token) {
                    struct aws_byte_cursor continuation_cur =
                        aws_byte_cursor_from_string(auto_ranged_put->synced_data.list_parts_continuation_token);
                    message_creation_result = aws_s3_construct_next_paginated_request_http_message(
                        auto_ranged_put->synced_data.list_parts_operation, &continuation_cur, &message);
                } else {
                    message_creation_result = aws_s3_construct_next_paginated_request_http_message(
                        auto_ranged_put->synced_data.list_parts_operation, NULL, &message);
                }

                aws_s3_meta_request_unlock_synced_data(meta_request);
            }
            /* END CRITICAL SECTION */

            if (message_creation_result) {
                goto message_create_failed;
            }
            if (meta_request->checksum_config.checksum_algorithm == AWS_SCA_NONE) {
                /* We don't need to worry about the pre-calculated checksum from user as for multipart upload, only way
                 * to calculate checksum for multipart upload is from client. */
                aws_s3_message_util_copy_headers(
                    meta_request->initial_request_message,
                    message,
                    g_s3_list_parts_excluded_headers,
                    g_s3_list_parts_excluded_headers_count,
                    true);
            } else {
                aws_s3_message_util_copy_headers(
                    meta_request->initial_request_message,
                    message,
                    g_s3_list_parts_with_checksum_excluded_headers,
                    g_s3_list_parts_with_checksum_excluded_headers_count,
                    true);
            }

            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD: {

            /* Create the message to create a new multipart upload. */
            message = aws_s3_create_multipart_upload_message_new(
                meta_request->allocator,
                meta_request->initial_request_message,
                meta_request->checksum_config.checksum_algorithm);

            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_PART: {

            size_t request_body_size = s_compute_request_body_size(meta_request, request->part_number);

            if (request->num_times_prepared == 0) {
                if (s_skip_parts_from_stream(
                        meta_request,
                        auto_ranged_put->prepare_data.num_parts_read_from_stream,
                        request->part_number - 1)) {
                    goto message_create_failed;
                }
                auto_ranged_put->prepare_data.num_parts_read_from_stream = request->part_number - 1;

                aws_byte_buf_init(&request->request_body, meta_request->allocator, request_body_size);

                if (aws_s3_meta_request_read_body(meta_request, &request->request_body)) {
                    goto message_create_failed;
                }
                ++auto_ranged_put->prepare_data.num_parts_read_from_stream;
            }
            /* Create a new put-object message to upload a part. */
            message = aws_s3_upload_part_message_new(
                meta_request->allocator,
                meta_request->initial_request_message,
                &request->request_body,
                request->part_number,
                auto_ranged_put->upload_id,
                meta_request->should_compute_content_md5,
                &meta_request->checksum_config,
                &auto_ranged_put->checksums_list[request->part_number - 1]);
            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD: {

            if (request->num_times_prepared == 0) {

                /* Corner case of last part being previosly uploaded during resume.
                 * Read it from input stream and potentially verify checksum */
                if (s_skip_parts_from_stream(
                        meta_request,
                        auto_ranged_put->prepare_data.num_parts_read_from_stream,
                        auto_ranged_put->synced_data.total_num_parts)) {
                    goto message_create_failed;
                }
                auto_ranged_put->prepare_data.num_parts_read_from_stream = auto_ranged_put->synced_data.total_num_parts;

                aws_byte_buf_init(
                    &request->request_body, meta_request->allocator, s_complete_multipart_upload_init_body_size_bytes);
            } else {
                aws_byte_buf_reset(&request->request_body, false);
            }

            /* BEGIN CRITICAL SECTION */
            {
                aws_s3_meta_request_lock_synced_data(meta_request);

                AWS_FATAL_ASSERT(auto_ranged_put->upload_id);
                AWS_ASSERT(request->request_body.capacity > 0);
                aws_byte_buf_reset(&request->request_body, false);

                /* Build the message to complete our multipart upload, which includes a payload describing all of
                 * our completed parts. */
                message = aws_s3_complete_multipart_message_new(
                    meta_request->allocator,
                    meta_request->initial_request_message,
                    &request->request_body,
                    auto_ranged_put->upload_id,
                    &auto_ranged_put->synced_data.etag_list,
                    auto_ranged_put->checksums_list,
                    meta_request->checksum_config.checksum_algorithm);

                aws_s3_meta_request_unlock_synced_data(meta_request);
            }
            /* END CRITICAL SECTION */

            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_ABORT_MULTIPART_UPLOAD: {
            AWS_FATAL_ASSERT(auto_ranged_put->upload_id);
            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "id=%p Abort multipart upload request for upload id %s.",
                (void *)meta_request,
                aws_string_c_str(auto_ranged_put->upload_id));

            if (request->num_times_prepared == 0) {
                aws_byte_buf_init(
                    &request->request_body, meta_request->allocator, s_abort_multipart_upload_init_body_size_bytes);
            } else {
                aws_byte_buf_reset(&request->request_body, false);
            }

            /* Build the message to abort our multipart upload */
            message = aws_s3_abort_multipart_upload_message_new(
                meta_request->allocator, meta_request->initial_request_message, auto_ranged_put->upload_id);

            break;
        }
    }

    if (message == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p Could not allocate message for request with tag %d for auto-ranged-put meta request.",
            (void *)meta_request,
            request->request_tag);
        goto message_create_failed;
    }

    aws_s3_request_setup_send_data(request, message);

    aws_http_message_release(message);

    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p: Prepared request %p for part %d",
        (void *)meta_request,
        (void *)request,
        request->part_number);

    return AWS_OP_SUCCESS;

message_create_failed:

    return AWS_OP_ERR;
}

static void s_s3_auto_ranged_put_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code) {

    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);
    AWS_PRECONDITION(request);

    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;

    switch (request->request_tag) {

        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_LIST_PARTS: {
            /* BEGIN CRITICAL SECTION */
            {
                aws_s3_meta_request_lock_synced_data(meta_request);

                bool has_more_results = false;

                if (error_code == AWS_ERROR_SUCCESS) {

                    struct aws_byte_cursor body_cursor = aws_byte_cursor_from_buf(&request->send_data.response_body);
                    aws_string_destroy(auto_ranged_put->synced_data.list_parts_continuation_token);
                    if (aws_s3_paginated_operation_on_response(
                            auto_ranged_put->synced_data.list_parts_operation,
                            &body_cursor,
                            &auto_ranged_put->synced_data.list_parts_continuation_token,
                            &has_more_results)) {
                        AWS_LOGF_ERROR(
                            AWS_LS_S3_META_REQUEST, "id=%p Failed to parse list parts response.", (void *)meta_request);
                        error_code = AWS_ERROR_S3_LIST_PARTS_PARSE_FAILED;
                    } else if (!has_more_results) {
                        for (size_t etag_index = 0;
                             etag_index < aws_array_list_length(&auto_ranged_put->synced_data.etag_list);
                             etag_index++) {
                            struct aws_string *etag = NULL;
                            aws_array_list_get_at(&auto_ranged_put->synced_data.etag_list, &etag, etag_index);
                            if (etag != NULL) {
                                /* Update the number of parts sent/completed previously */
                                ++auto_ranged_put->synced_data.num_parts_sent;
                                ++auto_ranged_put->synced_data.num_parts_completed;
                            }
                        }

                        AWS_LOGF_DEBUG(
                            AWS_LS_S3_META_REQUEST,
                            "id=%p: Resuming PutObject. %d out of %d parts have completed during previous request.",
                            (void *)meta_request,
                            auto_ranged_put->synced_data.num_parts_completed,
                            auto_ranged_put->synced_data.total_num_parts);
                    }
                }

                auto_ranged_put->synced_data.list_parts_completed = !has_more_results;
                auto_ranged_put->synced_data.create_multipart_upload_error_code = error_code;

                if (error_code != AWS_ERROR_SUCCESS) {
                    aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
                }

                aws_s3_meta_request_unlock_synced_data(meta_request);
            }
            /* END CRITICAL SECTION */
            break;
        }

        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD: {
            struct aws_http_headers *needed_response_headers = NULL;

            if (error_code == AWS_ERROR_SUCCESS) {
                needed_response_headers = aws_http_headers_new(meta_request->allocator);
                const size_t copy_header_count =
                    sizeof(s_create_multipart_upload_copy_headers) / sizeof(struct aws_byte_cursor);

                /* Copy any headers now that we'll need for the final, transformed headers later. */
                for (size_t header_index = 0; header_index < copy_header_count; ++header_index) {
                    const struct aws_byte_cursor *header_name = &s_create_multipart_upload_copy_headers[header_index];
                    struct aws_byte_cursor header_value;
                    AWS_ZERO_STRUCT(header_value);

                    if (!aws_http_headers_get(request->send_data.response_headers, *header_name, &header_value)) {
                        aws_http_headers_set(needed_response_headers, *header_name, header_value);
                    }
                }

                struct aws_byte_cursor buffer_byte_cursor = aws_byte_cursor_from_buf(&request->send_data.response_body);

                /* Find the upload id for this multipart upload. */
                struct aws_string *upload_id =
                    get_top_level_xml_tag_value(meta_request->allocator, &s_upload_id, &buffer_byte_cursor);

                if (upload_id == NULL) {
                    AWS_LOGF_ERROR(
                        AWS_LS_S3_META_REQUEST,
                        "id=%p Could not find upload-id in create-multipart-upload response",
                        (void *)meta_request);

                    aws_raise_error(AWS_ERROR_S3_MISSING_UPLOAD_ID);
                    error_code = AWS_ERROR_S3_MISSING_UPLOAD_ID;
                } else {
                    /* Store the multipart upload id. */
                    auto_ranged_put->upload_id = upload_id;
                }
            }

            /* BEGIN CRITICAL SECTION */
            {
                aws_s3_meta_request_lock_synced_data(meta_request);

                AWS_ASSERT(auto_ranged_put->synced_data.needed_response_headers == NULL)
                auto_ranged_put->synced_data.needed_response_headers = needed_response_headers;

                auto_ranged_put->synced_data.create_multipart_upload_completed = true;
                auto_ranged_put->synced_data.list_parts_error_code = error_code;

                if (error_code != AWS_ERROR_SUCCESS) {
                    aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
                }

                aws_s3_meta_request_unlock_synced_data(meta_request);
            }
            /* END CRITICAL SECTION */
            break;
        }

        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_PART: {
            size_t part_number = request->part_number;
            AWS_FATAL_ASSERT(part_number > 0);
            size_t part_index = part_number - 1;
            struct aws_string *etag = NULL;

            if (error_code == AWS_ERROR_SUCCESS) {
                /* Find the ETag header if it exists and cache it. */
                struct aws_byte_cursor etag_within_quotes;

                AWS_ASSERT(request->send_data.response_headers);

                if (aws_http_headers_get(
                        request->send_data.response_headers, g_etag_header_name, &etag_within_quotes)) {
                    AWS_LOGF_ERROR(
                        AWS_LS_S3_META_REQUEST,
                        "id=%p Could not find ETag header for request %p",
                        (void *)meta_request,
                        (void *)request);

                    error_code = AWS_ERROR_S3_MISSING_UPLOAD_ID;
                } else {
                    /* The ETag value arrives in quotes, but we don't want it in quotes when we send it back up
                     * later, so just get rid of the quotes now. */
                    etag = aws_strip_quotes(meta_request->allocator, etag_within_quotes);
                }
            }
            if (error_code == AWS_ERROR_SUCCESS && meta_request->progress_callback != NULL) {
                struct aws_s3_meta_request_progress progress = {
                    .bytes_transferred = meta_request->part_size,
                    .content_length = auto_ranged_put->content_length,
                };
                meta_request->progress_callback(meta_request, &progress, meta_request->user_data);
            }
            /* BEGIN CRITICAL SECTION */
            {
                aws_s3_meta_request_lock_synced_data(meta_request);

                ++auto_ranged_put->synced_data.num_parts_completed;

                AWS_LOGF_DEBUG(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p: %d out of %d parts have completed.",
                    (void *)meta_request,
                    auto_ranged_put->synced_data.num_parts_completed,
                    auto_ranged_put->synced_data.total_num_parts);

                if (error_code == AWS_ERROR_SUCCESS) {
                    AWS_ASSERT(etag != NULL);

                    ++auto_ranged_put->synced_data.num_parts_successful;

                    /* ETags need to be associated with their part number, so we keep the etag indices consistent with
                     * part numbers. This means we may have to add padding to the list in the case that parts finish out
                     * of order. */
                    aws_array_list_set_at(&auto_ranged_put->synced_data.etag_list, &etag, part_index);
                } else {
                    ++auto_ranged_put->synced_data.num_parts_failed;
                    aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
                }

                aws_s3_meta_request_unlock_synced_data(meta_request);
            }
            /* END CRITICAL SECTION */

            break;
        }

        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD: {
            if (error_code == AWS_ERROR_SUCCESS && meta_request->headers_callback != NULL) {
                struct aws_http_headers *final_response_headers = aws_http_headers_new(meta_request->allocator);

                /* Copy all the response headers from this request. */
                copy_http_headers(request->send_data.response_headers, final_response_headers);

                /* Copy over any response headers that we've previously determined are needed for this final
                 * response.
                 */

                /* BEGIN CRITICAL SECTION */
                {
                    aws_s3_meta_request_lock_synced_data(meta_request);
                    copy_http_headers(auto_ranged_put->synced_data.needed_response_headers, final_response_headers);
                    aws_s3_meta_request_unlock_synced_data(meta_request);
                }
                /* END CRITICAL SECTION */

                struct aws_byte_cursor response_body_cursor =
                    aws_byte_cursor_from_buf(&request->send_data.response_body);

                /**
                 * TODO: The body of the response can be ERROR, check Error specified in body part from
                 * https://docs.aws.amazon.com/AmazonS3/latest/API/API_CompleteMultipartUpload.html#AmazonS3-CompleteMultipartUpload-response-CompleteMultipartUploadOutput
                 * We need to handle this case.
                 * TODO: the checksum returned within the response of complete multipart upload need to be exposed?
                 */

                /* Grab the ETag for the entire object, and set it as a header. */
                struct aws_string *etag_header_value =
                    get_top_level_xml_tag_value(meta_request->allocator, &g_etag_header_name, &response_body_cursor);

                if (etag_header_value != NULL) {
                    struct aws_byte_buf etag_header_value_byte_buf;
                    AWS_ZERO_STRUCT(etag_header_value_byte_buf);

                    replace_quote_entities(meta_request->allocator, etag_header_value, &etag_header_value_byte_buf);

                    aws_http_headers_set(
                        final_response_headers,
                        g_etag_header_name,
                        aws_byte_cursor_from_buf(&etag_header_value_byte_buf));

                    aws_string_destroy(etag_header_value);
                    aws_byte_buf_clean_up(&etag_header_value_byte_buf);
                }

                /* Notify the user of the headers. */
                if (meta_request->headers_callback(
                        meta_request,
                        final_response_headers,
                        request->send_data.response_status,
                        meta_request->user_data)) {

                    error_code = aws_last_error_or_unknown();
                }
                meta_request->headers_callback = NULL;

                aws_http_headers_release(final_response_headers);
            }

            /* BEGIN CRITICAL SECTION */
            {
                aws_s3_meta_request_lock_synced_data(meta_request);
                auto_ranged_put->synced_data.complete_multipart_upload_completed = true;
                auto_ranged_put->synced_data.complete_multipart_upload_error_code = error_code;

                if (error_code != AWS_ERROR_SUCCESS) {
                    aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
                }
                aws_s3_meta_request_unlock_synced_data(meta_request);
            }
            /* END CRITICAL SECTION */

            break;
        }
        case AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_ABORT_MULTIPART_UPLOAD: {
            /* BEGIN CRITICAL SECTION */
            {
                aws_s3_meta_request_lock_synced_data(meta_request);
                auto_ranged_put->synced_data.abort_multipart_upload_error_code = error_code;
                auto_ranged_put->synced_data.abort_multipart_upload_completed = true;
                aws_s3_meta_request_unlock_synced_data(meta_request);
            }
            /* END CRITICAL SECTION */
            break;
        }
    }
}

static int s_s3_auto_ranged_put_pause(struct aws_s3_meta_request *meta_request, struct aws_string **out_resume_token) {

    /* lock */
    aws_s3_meta_request_lock_synced_data(meta_request);
    struct aws_s3_auto_ranged_put *auto_ranged_put = meta_request->impl;

    /* only generate token if multipart upload was completed, since there is nothing to resume otherwise*/
    int token_generated_error = AWS_OP_SUCCESS;
    *out_resume_token = NULL;
    if (auto_ranged_put->synced_data.create_multipart_upload_completed) {
        struct aws_json_value *root = aws_json_value_new_object(meta_request->allocator);

        /**
         * generate pause token for put.
         * current format is json with the following structure
         * {
         *      type: String
         *      multipart_upload_id: String
         *      partition_size: number
         *      total_num_parts: number
         * }
         */

        aws_json_value_add_to_object(
            root,
            aws_byte_cursor_from_c_str("type"),
            aws_json_value_new_string(
                meta_request->allocator, aws_byte_cursor_from_c_str("AWS_S3_META_REQUEST_TYPE_PUT_OBJECT")));

        aws_json_value_add_to_object(
            root,
            aws_byte_cursor_from_c_str("multipart_upload_id"),
            aws_json_value_new_string(
                meta_request->allocator, aws_byte_cursor_from_string(auto_ranged_put->upload_id)));

        aws_json_value_add_to_object(
            root,
            aws_byte_cursor_from_c_str("partition_size"),
            aws_json_value_new_number(meta_request->allocator, (double)meta_request->part_size));

        aws_json_value_add_to_object(
            root,
            aws_byte_cursor_from_c_str("total_num_parts"),
            aws_json_value_new_number(meta_request->allocator, (double)auto_ranged_put->synced_data.total_num_parts));

        struct aws_byte_buf result_string_buf;
        aws_byte_buf_init(&result_string_buf, meta_request->allocator, 0);

        token_generated_error = aws_byte_buf_append_json_string(root, &result_string_buf);

        if (!token_generated_error) {
            *out_resume_token = aws_string_new_from_buf(meta_request->allocator, &result_string_buf);
        }

        aws_byte_buf_clean_up(&result_string_buf);
        aws_json_value_destroy(root);
    }

    /**
     * Cancels the meta request using the PAUSED flag to avoid deletion of uploaded parts.
     * This allows the client to resume the upload later, setting the persistable state in the meta request options.
     */
    aws_s3_meta_request_set_fail_synced(meta_request, NULL, AWS_ERROR_S3_PAUSED);

    /* unlock */
    aws_s3_meta_request_unlock_synced_data(meta_request);

    return token_generated_error;
}
