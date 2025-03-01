#ifndef AWS_S3_CLIENT_IMPL_H
#define AWS_S3_CLIENT_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/s3_client.h"

#include <aws/common/atomics.h>
#include <aws/common/byte_buf.h>
#include <aws/common/hash_table.h>
#include <aws/common/linked_list.h>
#include <aws/common/mutex.h>
#include <aws/common/ref_count.h>
#include <aws/common/task_scheduler.h>
#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/proxy.h>

/* TODO automate this value in the future to prevent it from becoming out-of-sync. */
#define AWS_S3_CLIENT_VERSION "0.1.x"

struct aws_http_connection;
struct aws_http_connection_manager;
struct aws_host_resolver;
struct aws_s3_endpoint;

enum aws_s3_connection_finish_code {
    AWS_S3_CONNECTION_FINISH_CODE_SUCCESS,
    AWS_S3_CONNECTION_FINISH_CODE_FAILED,
    AWS_S3_CONNECTION_FINISH_CODE_RETRY,
};

/* Callback for the owner of the endpoint when the endpoint has completely cleaned up. */
typedef void(aws_s3_endpoint_shutdown_fn)(void *user_data);

struct aws_s3_endpoint_options {
    /* URL of the host that this endpoint refers to. */
    struct aws_string *host_name;

    /* Callback for when this endpoint completely shuts down. */
    aws_s3_endpoint_shutdown_fn *shutdown_callback;

    /* Bootstrap of the client to be used for spawning a connection manager. */
    struct aws_client_bootstrap *client_bootstrap;

    /* TLS connection options to be used for the connection manager. */
    const struct aws_tls_connection_options *tls_connection_options;

    /* DNS TTL to use for addresses for this endpoint. */
    size_t dns_host_address_ttl_seconds;

    /* Client that owns this endpoint */
    struct aws_s3_client *client;

    /* Maximum number of connections that can be spawned for this endpoint. */
    uint32_t max_connections;

    /* HTTP port override. If zero, determine port based on TLS context */
    uint16_t port;

    /**
     * Optional.
     * Proxy configuration for http connection.
     */
    struct aws_http_proxy_config *proxy_config;

    /**
     * Optional.
     * Configuration for fetching proxy configuration from environment.
     * By Default proxy_ev_settings.aws_http_proxy_env_var_type is set to AWS_HPEV_ENABLE which means read proxy
     * configuration from environment.
     * Only works when proxy_config is not set. If both are set, configuration from proxy_config is used.
     */
    struct proxy_env_var_settings *proxy_ev_settings;

    /**
     * Optional.
     * If set to 0, default value is used.
     */
    uint32_t connect_timeout_ms;

    /**
     * Optional.
     * Set keepalive to periodically transmit messages for detecting a disconnected peer.
     */
    struct aws_s3_tcp_keep_alive_options *tcp_keep_alive_options;

    /**
     * Optional.
     * Configuration options for connection monitoring.
     * If the transfer speed falls below the specified minimum_throughput_bytes_per_second, the operation is aborted.
     */
    struct aws_http_connection_monitoring_options *monitoring_options;
};

/* global vtable, only used when mocking for tests */
struct aws_s3_endpoint_system_vtable {
    void (*acquire)(struct aws_s3_endpoint *endpoint, bool already_holding_lock);
    void (*release)(struct aws_s3_endpoint *endpoint);
};

struct aws_s3_endpoint {
    struct {
        /* This is NOT an atomic ref-count.
         * The endpoint lives in hashtable: `aws_s3_client.synced_data.endpoints`
         * This ref-count can only be touched while holding client's lock */
        size_t ref_count;
    } client_synced_data;

    /* What allocator was used to create this endpoint. */
    struct aws_allocator *allocator;

    /* URL of the host that this endpoint refers to. */
    struct aws_string *host_name;

    /* Connection manager that manages all connections to this endpoint. */
    struct aws_http_connection_manager *http_connection_manager;

    /* Client that owns this endpoint */
    struct aws_s3_client *client;
};

/* Represents one connection on a particular VIP. */
struct aws_s3_connection {
    /* Endpoint that this connection is connected to. */
    struct aws_s3_endpoint *endpoint;

    /* The underlying, currently in-use HTTP connection. */
    struct aws_http_connection *http_connection;

    /* Request currently being processed on this connection. */
    struct aws_s3_request *request;

    /* Current retry token for the request. If it has never been retried, this will be NULL. */
    struct aws_retry_token *retry_token;
};

struct aws_s3_client_vtable {

    struct aws_s3_meta_request *(
        *meta_request_factory)(struct aws_s3_client *client, const struct aws_s3_meta_request_options *options);

    void (*create_connection_for_request)(struct aws_s3_client *client, struct aws_s3_request *request);

    void (*acquire_http_connection)(
        struct aws_http_connection_manager *conn_manager,
        aws_http_connection_manager_on_connection_setup_fn *on_connection_acquired_callback,
        void *user_data);

    size_t (*get_host_address_count)(
        struct aws_host_resolver *host_resolver,
        const struct aws_string *host_name,
        uint32_t flags);

    void (*schedule_process_work_synced)(struct aws_s3_client *client);

    void (*process_work)(struct aws_s3_client *client);

    void (*endpoint_shutdown_callback)(struct aws_s3_client *client);

    void (*finish_destroy)(struct aws_s3_client *client);
};

/* Represents the state of the S3 client. */
struct aws_s3_client {
    struct aws_allocator *allocator;

    struct aws_s3_client_vtable *vtable;

    struct aws_ref_count ref_count;

    /* Client bootstrap for setting up connection managers. */
    struct aws_client_bootstrap *client_bootstrap;

    /* Event loop on the client bootstrap ELG for processing work/dispatching requests. */
    struct aws_event_loop *process_work_event_loop;

    /* Event loop group for streaming request bodies back to the user. */
    struct aws_event_loop_group *body_streaming_elg;

    /* Region of the S3 bucket. */
    struct aws_string *region;

    /* Size of parts for files when doing gets or puts.  This exists on the client as configurable option that is passed
     * to meta requests for use. */
    const size_t part_size;

    /* Size of parts for files when doing gets or puts.  This exists on the client as configurable option that is passed
     * to meta requests for use. */
    const size_t max_part_size;

    /* TLS Options to be used for each connection. */
    struct aws_tls_connection_options *tls_connection_options;

    /* Cached signing config. Can be NULL if no signing config was specified. */
    struct aws_cached_signing_config_aws *cached_signing_config;

    /* Throughput target in Gbps that we are trying to reach. */
    const double throughput_target_gbps;

    /* The calculated ideal number of VIP's based on throughput target and throughput per vip. */
    const uint32_t ideal_vip_count;

    /**
     * For multi-part upload, content-md5 will be calculated if the AWS_MR_CONTENT_MD5_ENABLED is specified
     *     or initial request has content-md5 header.
     * For single-part upload, if the content-md5 header is specified, it will remain unchanged. If the header is not
     *     specified, and this is set to AWS_MR_CONTENT_MD5_ENABLED, it will be calculated. */
    const enum aws_s3_meta_request_compute_content_md5 compute_content_md5;

    /* Hard limit on max connections set through the client config. */
    const uint32_t max_active_connections_override;

    struct aws_atomic_var max_allowed_connections;

    /* Retry strategy used for scheduling request retries. */
    struct aws_retry_strategy *retry_strategy;

    /**
     * Optional.
     * Proxy configuration for http connection.
     */
    struct aws_http_proxy_config *proxy_config;

    /**
     * Optional.
     * Configuration for fetching proxy configuration from environment.
     * By Default proxy_ev_settings.aws_http_proxy_env_var_type is set to AWS_HPEV_ENABLE which means read proxy
     * configuration from environment.
     * Only works when proxy_config is not set. If both are set, configuration from proxy_config is used.
     */
    struct proxy_env_var_settings *proxy_ev_settings;

    /**
     * Optional.
     * If set to 0, default value is used.
     */
    uint32_t connect_timeout_ms;

    /**
     * Optional.
     * Set keepalive to periodically transmit messages for detecting a disconnected peer.
     */
    struct aws_s3_tcp_keep_alive_options *tcp_keep_alive_options;

    /**
     * Optional.
     * Configuration options for connection monitoring.
     * If the transfer speed falls below the specified minimum_throughput_bytes_per_second, the operation is aborted.
     */
    struct aws_http_connection_monitoring_options *monitoring_options;

    /* tls options from proxy environment settings. */
    struct aws_tls_connection_options *proxy_ev_tls_options;

    /* Shutdown callbacks to notify when the client is completely cleaned up. */
    aws_s3_client_shutdown_complete_callback_fn *shutdown_callback;
    void *shutdown_callback_user_data;

    /* Whether read backpressure (aka flow-control window) is being applied. */
    const bool enable_read_backpressure;

    /* The starting size of each meta request's flow-control window, in bytes.
     * Ignored unless `enable_read_backpressure` is true. */
    const size_t initial_read_window;

    struct {
        /* Number of overall requests currently being processed by the client. */
        struct aws_atomic_var num_requests_in_flight;

        /* Number of requests being sent/received over network. */
        struct aws_atomic_var num_requests_network_io[AWS_S3_META_REQUEST_TYPE_MAX];

        /* Number of requests sitting in their meta request priority queue, waiting to be streamed. */
        struct aws_atomic_var num_requests_stream_queued_waiting;

        /* Number of requests currently scheduled to be streamed or are actively being streamed. */
        struct aws_atomic_var num_requests_streaming;
    } stats;

    struct {
        struct aws_mutex lock;

        /* Hash table of endpoints that are in-use by the client.
         * Key: aws_string of endpoint hostname. Value: aws_s3_endpoint */
        struct aws_hash_table endpoints;

        /* How many requests failed to be prepared. */
        uint32_t num_failed_prepare_requests;

        /* Meta requests that need added in the work event loop.
         * List contains aws_s3_meta_request_work */
        struct aws_linked_list pending_meta_request_work;

        /* aws_s3_request that are prepared and ready to be put in the threaded_data request queue. */
        struct aws_linked_list prepared_requests;

        /* Task for processing requests from meta requests on connections. */
        struct aws_task process_work_task;

        /* Number of endpoints currently allocated. Used during clean up to know how many endpoints are still in
         * memory.*/
        uint32_t num_endpoints_allocated;

        /* Whether or not the client has started cleaning up all of its resources */
        uint32_t active : 1;

        /* True if the start_destroy function is still executing, which blocks shutdown from completing. */
        uint32_t start_destroy_executing : 1;

        /* Whether or not work processing is currently scheduled. */
        uint32_t process_work_task_scheduled : 1;

        /* Whether or not work process is currently in progress. */
        uint32_t process_work_task_in_progress : 1;

        /* Whether or not the body streaming ELG is allocated. If the body streaming ELG is NULL, but this is true, the
         * shutdown callback has not yet been called.*/
        uint32_t body_streaming_elg_allocated : 1;

        /* True if client has been flagged to finish destroying itself. Used to catch double-destroy bugs.*/
        uint32_t finish_destroy : 1;

    } synced_data;

    struct {
        /* Queue of prepared aws_s3_request that are waiting to be assigned to connections. */
        struct aws_linked_list request_queue;

        /* Client list of ongoing aws_s3_meta_requests. */
        struct aws_linked_list meta_requests;

        /* Number of requests in the request_queue linked_list. */
        uint32_t request_queue_size;

        /* Number of requests currently being prepared. */
        uint32_t num_requests_being_prepared;

    } threaded_data;
};

void aws_s3_client_notify_connection_finished(
    struct aws_s3_client *client,
    struct aws_s3_connection *connection,
    int error_code,
    enum aws_s3_connection_finish_code finish_code);

void aws_s3_client_notify_request_destroyed(struct aws_s3_client *client, struct aws_s3_request *request);

AWS_EXTERN_C_BEGIN

AWS_S3_API
void aws_s3_set_dns_ttl(size_t ttl);

AWS_S3_API
uint32_t aws_s3_client_get_max_requests_prepare(struct aws_s3_client *client);

AWS_S3_API
uint32_t aws_s3_client_get_max_active_connections(
    struct aws_s3_client *client,
    struct aws_s3_meta_request *meta_request);

AWS_S3_API
uint32_t aws_s3_client_get_max_requests_in_flight(struct aws_s3_client *client);

AWS_S3_API
uint32_t aws_s3_client_queue_requests_threaded(
    struct aws_s3_client *client,
    struct aws_linked_list *request_list,
    bool queue_front);

AWS_S3_API
struct aws_s3_request *aws_s3_client_dequeue_request_threaded(struct aws_s3_client *client);

AWS_S3_API
void aws_s3_client_schedule_process_work(struct aws_s3_client *client);

AWS_S3_API
void aws_s3_client_update_meta_requests_threaded(struct aws_s3_client *client);

AWS_S3_API
void aws_s3_client_update_connections_threaded(struct aws_s3_client *client);

AWS_S3_API
struct aws_s3_endpoint *aws_s3_endpoint_new(
    struct aws_allocator *allocator,
    const struct aws_s3_endpoint_options *options);

AWS_S3_API void aws_s3_client_lock_synced_data(struct aws_s3_client *client);

AWS_S3_API
void aws_s3_client_unlock_synced_data(struct aws_s3_client *client);

/* Used for mocking */
AWS_S3_API
void aws_s3_endpoint_set_system_vtable(const struct aws_s3_endpoint_system_vtable *vtable);

/* Increment the endpoint's ref-count.
 * If `already_holding_lock` is false, then this call will briefly take hold of the client's lock */
struct aws_s3_endpoint *aws_s3_endpoint_acquire(struct aws_s3_endpoint *endpoint, bool already_holding_lock);

/* Decrement the endpoint's ref-count.
 * You MUST NOT call this while the client's lock is held.
 * (this call briefly holds the client's lock and may remove the endpoint
 * from the client's hashtable) */
void aws_s3_endpoint_release(struct aws_s3_endpoint *endpoint);

AWS_S3_API
extern const uint32_t g_max_num_connections_per_vip;

AWS_S3_API
extern const uint32_t g_num_conns_per_vip_meta_request_look_up[];

AWS_EXTERN_C_END

#endif /* AWS_S3_CLIENT_IMPL_H */
