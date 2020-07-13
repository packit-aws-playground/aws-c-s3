#ifndef AWS_S3_CLIENT_IMPL_H
#define AWS_S3_CLIENT_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/s3_client.h"
#include <aws/common/atomics.h>

struct aws_s3_client {
    struct aws_allocator *allocator;
    struct aws_atomic_var ref_count;

    struct aws_string *region;
    struct aws_string *bucket_name;
    struct aws_string *endpoint;
    struct aws_client_bootstrap *client_bootstrap;
    struct aws_credentials_provider *credentials_provider;
    struct aws_http_connection_manager *connection_manager;

    aws_s3_client_shutdown_complete_callback *shutdown_callback;
    void *shutdown_callback_user_data;
    struct aws_atomic_var shutdown_wait_count;
};

#endif /* AWS_S3_CLIENT_IMPL_H */
