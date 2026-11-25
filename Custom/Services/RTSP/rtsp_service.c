/**
 * @file rtsp_service.c
 * @brief RTSP Service implementation - lifecycle, video_hub subscription, client management
 *
 * Uses a single server task with select() to multiplex all client connections,
 * avoiding per-client thread creation that can exhaust ThreadX TCB pool.
 */

#include "rtsp_service.h"
#include "rtsp_server.h"
#include "rtsp_rtp.h"
#include "Services/Video/video_stream_hub.h"
#include "aicam_types.h"
#include "aicam_error.h"
#include "debug.h"
#include "cmsis_os2.h"
#include "service_init.h"
#include "json_config_mgr.h"
#include "common_utils.h"
#include "Hal/Network/netif_manager/netif_manager.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "Core/Data/buffer_mgr.h"

/* ==================== Configuration ==================== */

#define RTSP_TASK_PRIORITY          osPriorityNormal
#define RTSP_SELECT_TIMEOUT_MS      1000
#define RTSP_RECV_BUF_SIZE          2048

/* ==================== Service Context ==================== */

typedef struct {
    aicam_bool_t initialized;
    aicam_bool_t running;
    service_state_t service_state;

    /* Network */
    int listen_socket;
    uint16_t listen_port;
    osThreadId_t server_task_handle;
    uint8_t server_task_stack[RTSP_TASK_STACK_SIZE] AICAM_ALIGN_32;

    /* Clients */
    rtsp_client_t clients[RTSP_MAX_CLIENTS];
    osMutexId_t mutex;

    /* Video Hub */
    video_hub_subscriber_id_t hub_subscriber_id;
    aicam_bool_t hub_subscribed;

    /* SPS/PPS cache */
    uint8_t sps_data[256];
    uint32_t sps_size;
    uint8_t pps_data[64];
    uint32_t pps_size;
    aicam_bool_t sps_pps_valid;

    /* Auth config */
    rtsp_service_auth_config_t auth_config;

    /* Device IP cache */
    char device_ip[64];
} rtsp_service_ctx_t;

static rtsp_service_ctx_t g_rtsp_ctx;

/* ==================== Async send queue (mirrors the RTMP service) ====================
 * rtsp_on_frame runs on the ENCODER thread (hub callback); doing socket I/O
 * there lets one stalled RTSP client stall the whole encode pipeline (and a
 * bare blocking send() can wedge forever on a full TCP window). Instead the
 * callback only enqueues a frame copy; a dedicated send task owns all sends
 * with select()-guarded writes. Queue-full overwrites the OLDEST frame and
 * flags a drop - consumers then wait for the next keyframe instead of
 * decoding a broken reference chain (green/smeared screen). */
#define RTSP_SEND_QUEUE_SIZE        4
#define RTSP_MAX_FRAME_SIZE         (256*1024)
#define RTSP_SEND_TASK_STACK_SIZE   (4096*2)
#define RTSP_SEND_ERROR_LIMIT       5

typedef struct {
    uint8_t *data;
    uint32_t size;
    aicam_bool_t is_keyframe;
    aicam_bool_t valid;
} rtsp_frame_entry_t;

typedef struct {
    rtsp_frame_entry_t frames[RTSP_SEND_QUEUE_SIZE];
    volatile uint32_t head;             /* write position */
    volatile uint32_t tail;             /* read position */
    osMutexId_t mutex;
    osSemaphoreId_t sem_data;
    uint8_t *frame_buffers[RTSP_SEND_QUEUE_SIZE]; /* lazy aligned allocs */
    volatile uint8_t dropped;           /* set on overwrite, cleared by task */
    uint32_t drop_count;
} rtsp_send_queue_t;

static rtsp_send_queue_t s_send_q;
static osThreadId_t s_send_task_handle = NULL;
static uint8_t s_send_task_stack[RTSP_SEND_TASK_STACK_SIZE] AICAM_ALIGN_32;
/* Send-side staging copy: send_queue_pop hands out a POINTER into the ring
 * slot - if a slow client keeps the task busy past one ring wrap the
 * producer overwrites that buffer mid-send and the client decodes a
 * half-old/half-new frame (flash-back + smear until the next IDR). The
 * task sends from this private copy instead. */
static uint8_t s_send_staging[RTSP_MAX_FRAME_SIZE] AICAM_ALIGN_32;

static void rtsp_send_task(void *argument);

static aicam_bool_t send_queue_init(void)
{
    memset(&s_send_q, 0, sizeof(s_send_q));
    s_send_q.mutex = osMutexNew(NULL);
    s_send_q.sem_data = osSemaphoreNew(RTSP_SEND_QUEUE_SIZE, 0, NULL);
    if (!s_send_q.mutex || !s_send_q.sem_data) {
        return AICAM_FALSE;
    }
    for (int i = 0; i < RTSP_SEND_QUEUE_SIZE; i++) {
        s_send_q.frame_buffers[i] = buffer_malloc_aligned(RTSP_MAX_FRAME_SIZE, 32);
        if (!s_send_q.frame_buffers[i]) {
            LOG_SVC_ERROR("RTSP: send queue alloc failed (slot %d)", i);
            return AICAM_FALSE;
        }
        s_send_q.frames[i].data = s_send_q.frame_buffers[i];
    }
    return AICAM_TRUE;
}

static void send_queue_deinit(void)
{
    for (int i = 0; i < RTSP_SEND_QUEUE_SIZE; i++) {
        if (s_send_q.frame_buffers[i]) {
            buffer_free(s_send_q.frame_buffers[i]);
            s_send_q.frame_buffers[i] = NULL;
        }
    }
    if (s_send_q.sem_data) { osSemaphoreDelete(s_send_q.sem_data); s_send_q.sem_data = NULL; }
    if (s_send_q.mutex) { osMutexDelete(s_send_q.mutex); s_send_q.mutex = NULL; }
}

/* Non-blocking; overwrites the OLDEST unread entry when full. */
static aicam_result_t send_queue_push(const uint8_t *data, uint32_t size,
                                      aicam_bool_t is_keyframe)
{
    if (size > RTSP_MAX_FRAME_SIZE) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    osMutexAcquire(s_send_q.mutex, osWaitForever);
    uint32_t write_pos = s_send_q.head % RTSP_SEND_QUEUE_SIZE;
    rtsp_frame_entry_t *entry = &s_send_q.frames[write_pos];
    if (entry->valid) {
        s_send_q.dropped = 1;
        s_send_q.drop_count++;
    }
    memcpy(entry->data, data, size);
    entry->size = size;
    entry->is_keyframe = is_keyframe;
    entry->valid = AICAM_TRUE;
    s_send_q.head++;
    osMutexRelease(s_send_q.mutex);
    osSemaphoreRelease(s_send_q.sem_data);
    return AICAM_OK;
}

/* Blocking with timeout; AICAM_ERROR_TIMEOUT when empty. */
static aicam_result_t send_queue_pop(rtsp_frame_entry_t *out)
{
    if (osSemaphoreAcquire(s_send_q.sem_data, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    osMutexAcquire(s_send_q.mutex, osWaitForever);
    uint32_t read_pos = s_send_q.tail % RTSP_SEND_QUEUE_SIZE;
    rtsp_frame_entry_t *entry = &s_send_q.frames[read_pos];
    if (!entry->valid) {
        osMutexRelease(s_send_q.mutex);
        return AICAM_ERROR_TIMEOUT;
    }
    *out = *entry;
    entry->valid = AICAM_FALSE;
    s_send_q.tail++;
    osMutexRelease(s_send_q.mutex);
    return AICAM_OK;
}

/* ==================== Forward Declarations ==================== */

static void rtsp_server_task(void *arg);
static aicam_result_t rtsp_on_frame(const video_hub_frame_t *frame, void *user_data);
static void rtsp_on_sps_pps(const video_hub_sps_pps_t *sps_pps, void *user_data);
static void rtsp_release_client(rtsp_client_t *client);
static rtsp_client_t* rtsp_find_client_by_session(const char *session_id);
static void rtsp_accept_client(void);
static void rtsp_handle_client_data(rtsp_client_t *client);

/* ==================== Configuration Loading ==================== */

static void rtsp_load_config(void)
{
    video_stream_mode_config_t vs_config;
    json_config_get_video_stream_mode(&vs_config);

    g_rtsp_ctx.listen_port = vs_config.rtsp_port ? vs_config.rtsp_port : RTSP_DEFAULT_PORT;

    strncpy(g_rtsp_ctx.auth_config.auth_mode,
            vs_config.rtsp_auth_mode[0] ? vs_config.rtsp_auth_mode : "none",
            sizeof(g_rtsp_ctx.auth_config.auth_mode) - 1);
    strncpy(g_rtsp_ctx.auth_config.username,
            vs_config.rtsp_username[0] ? vs_config.rtsp_username : "",
            sizeof(g_rtsp_ctx.auth_config.username) - 1);
    strncpy(g_rtsp_ctx.auth_config.password,
            vs_config.rtsp_password[0] ? vs_config.rtsp_password : "",
            sizeof(g_rtsp_ctx.auth_config.password) - 1);
}

/* ==================== Device IP ==================== */

static void rtsp_update_device_ip(void)
{
    netif_info_t info;
    memset(&info, 0, sizeof(info));

    if (nm_ctrl_get_default_netif_info(&info) == 0) {
        snprintf(g_rtsp_ctx.device_ip, sizeof(g_rtsp_ctx.device_ip),
                 "%u.%u.%u.%u",
                 info.ip_addr[0], info.ip_addr[1],
                 info.ip_addr[2], info.ip_addr[3]);
    } else {
        g_rtsp_ctx.device_ip[0] = '\0';
    }
}

/* ==================== Service Lifecycle ==================== */

aicam_result_t rtsp_service_init(void *config)
{
    (void)config;
    memset(&g_rtsp_ctx, 0, sizeof(g_rtsp_ctx));

    g_rtsp_ctx.listen_socket = -1;
    g_rtsp_ctx.service_state = SERVICE_STATE_UNINITIALIZED;
    g_rtsp_ctx.hub_subscriber_id = VIDEO_HUB_INVALID_SUBSCRIBER_ID;

    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        g_rtsp_ctx.clients[i].tcp_socket = -1;
        g_rtsp_ctx.clients[i].rtp_socket = -1;
        g_rtsp_ctx.clients[i].in_use = AICAM_FALSE;
        g_rtsp_ctx.clients[i].ssrc = (uint32_t)(rand() + i * 1000);
    }

    g_rtsp_ctx.mutex = osMutexNew(NULL);
    if (!g_rtsp_ctx.mutex) {
        LOG_SVC_ERROR("RTSP: Failed to create mutex");
        return AICAM_ERROR;
    }

    rtsp_load_config();

    g_rtsp_ctx.initialized = AICAM_TRUE;
    g_rtsp_ctx.service_state = SERVICE_STATE_INITIALIZED;
    LOG_SVC_INFO("RTSP service initialized (port=%lu, auth=%s)",
                 (unsigned long)g_rtsp_ctx.listen_port, g_rtsp_ctx.auth_config.auth_mode);
    return AICAM_OK;
}

aicam_result_t rtsp_service_start(void)
{
    if (!g_rtsp_ctx.initialized) {
        LOG_SVC_ERROR("RTSP: start called but not initialized");
        return AICAM_ERROR_NOT_INITIALIZED;
    }

    if (g_rtsp_ctx.running) {
        LOG_SVC_INFO("RTSP: already running");
        return AICAM_OK;
    }

    /* Defensive cleanup: if a stale server task exists from a failed prior
     * stop, wait for it to exit.  The task closes all sockets itself before
     * returning, so we don't touch any sockets here.  Use polling instead
     * of osThreadJoin to avoid blocking forever if the task is stuck. */
    if (g_rtsp_ctx.server_task_handle) {
        LOG_SVC_WARN("RTSP: stale server task found, cleaning up");
        g_rtsp_ctx.running = AICAM_FALSE;
        uint32_t wait_start = osKernelGetTickCount();
        do {
            osDelay(100);
            osThreadState_t state = osThreadGetState(g_rtsp_ctx.server_task_handle);
            if (state == osThreadTerminated || state == osThreadInactive) {
                break;
            }
        } while ((osKernelGetTickCount() - wait_start) < 3000);
        osThreadTerminate(g_rtsp_ctx.server_task_handle);
        g_rtsp_ctx.server_task_handle = NULL;
    }

    /* Safety net: if listen_socket is somehow still open, close it now.
     * This can happen if the previous task was killed before cleanup. */
    if (g_rtsp_ctx.listen_socket >= 0) {
        LOG_SVC_WARN("RTSP: stale listen socket %d found, closing", g_rtsp_ctx.listen_socket);
        close(g_rtsp_ctx.listen_socket);
        g_rtsp_ctx.listen_socket = -1;
    }
    if (g_rtsp_ctx.hub_subscribed && g_rtsp_ctx.hub_subscriber_id >= 0) {
        video_hub_unsubscribe(g_rtsp_ctx.hub_subscriber_id);
        g_rtsp_ctx.hub_subscriber_id = VIDEO_HUB_INVALID_SUBSCRIBER_ID;
        g_rtsp_ctx.hub_subscribed = AICAM_FALSE;
    }
    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (g_rtsp_ctx.clients[i].in_use) {
            g_rtsp_ctx.clients[i].playing = AICAM_FALSE;
            rtsp_release_client(&g_rtsp_ctx.clients[i]);
        }
    }
    osMutexRelease(g_rtsp_ctx.mutex);

    rtsp_load_config();

    video_stream_mode_config_t vs_config;
    json_config_get_video_stream_mode(&vs_config);
    LOG_SVC_INFO("RTSP: start check: rtsp_enable=%d, rtsp_port=%lu, rtmp_enable=%d",
                 vs_config.rtsp_enable, (unsigned long)vs_config.rtsp_port, vs_config.rtmp_enable);

    if (!vs_config.rtsp_enable) {
        LOG_SVC_INFO("RTSP: Disabled in config, not starting listener");
        return AICAM_OK;
    }

    rtsp_update_device_ip();
    LOG_SVC_INFO("RTSP: device IP=%s", g_rtsp_ctx.device_ip[0] ? g_rtsp_ctx.device_ip : "UNKNOWN");

    LOG_SVC_INFO("RTSP: video_hub state: init=%d, subscribers=%lu",
                 video_hub_is_initialized(), (unsigned long)video_hub_get_subscriber_count());

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOG_SVC_ERROR("RTSP: Failed to create socket, errno=%d", errno);
        return AICAM_ERROR;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_rtsp_ctx.listen_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int bind_errno = errno;
        LOG_SVC_ERROR("RTSP: Failed to bind port %lu (errno=%d, sock=%d)",
                      (unsigned long)g_rtsp_ctx.listen_port, bind_errno, sock);
        close(sock);
        /* If port is still in transition (TIME_WAIT despite SO_REUSEADDR),
         * retry once after a short delay. */
        if (bind_errno == EADDRINUSE || bind_errno == EHOSTDOWN) {
            LOG_SVC_INFO("RTSP: retrying bind in 500ms...");
            osDelay(500);
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    LOG_SVC_INFO("RTSP: bind retry succeeded");
                    goto bind_ok;
                }
                LOG_SVC_ERROR("RTSP: bind retry also failed (errno=%d)", errno);
                close(sock);
            }
        }
        return AICAM_ERROR;
    }
bind_ok:

    if (listen(sock, RTSP_MAX_CLIENTS) < 0) {
        LOG_SVC_ERROR("RTSP: Failed to listen");
        close(sock);
        return AICAM_ERROR;
    }

    g_rtsp_ctx.listen_socket = sock;
    LOG_SVC_INFO("RTSP: TCP listening on port %lu, sock=%d",
                 (unsigned long)g_rtsp_ctx.listen_port, sock);

    /* Subscribe to video hub BEFORE starting task, so encoder starts producing frames.
     * Without subscribers, video_encoder_node skips frame injection. */
    LOG_SVC_INFO("RTSP: subscribing to video_hub (VIDEO_HUB_SUBSCRIBER_RTSP=%d)...",
                 VIDEO_HUB_SUBSCRIBER_RTSP);

    g_rtsp_ctx.hub_subscriber_id = video_hub_subscribe(
        VIDEO_HUB_SUBSCRIBER_RTSP,
        rtsp_on_frame,
        rtsp_on_sps_pps,
        &g_rtsp_ctx
    );

    if (g_rtsp_ctx.hub_subscriber_id >= 0) {
        g_rtsp_ctx.hub_subscribed = AICAM_TRUE;
        LOG_SVC_INFO("RTSP: Subscribed to video hub OK (id=%d, hub_subscribers=%lu)",
                     g_rtsp_ctx.hub_subscriber_id,
                     (unsigned long)video_hub_get_subscriber_count());
    } else {
        LOG_SVC_ERROR("RTSP: Failed to subscribe to video hub (id=%d)", g_rtsp_ctx.hub_subscriber_id);
    }

    /* Set running flag BEFORE creating task - the task checks this flag on entry */
    g_rtsp_ctx.running = AICAM_TRUE;
    g_rtsp_ctx.service_state = SERVICE_STATE_RUNNING;

    osThreadAttr_t task_attr = {
        .name = "rtsp_server",
        .stack_size = RTSP_TASK_STACK_SIZE,
        .stack_mem = g_rtsp_ctx.server_task_stack,
        .priority = RTSP_TASK_PRIORITY,
    };

    g_rtsp_ctx.server_task_handle = osThreadNew(rtsp_server_task, NULL, &task_attr);
    if (!g_rtsp_ctx.server_task_handle) {
        LOG_SVC_ERROR("RTSP: Failed to create server task");
        g_rtsp_ctx.running = AICAM_FALSE;
        close(sock);
        g_rtsp_ctx.listen_socket = -1;
        return AICAM_ERROR;
    }

    /* Async send queue + dedicated send task (frame sends must never run
     * on the encoder thread - see the queue block above). */
    if (!send_queue_init()) {
        LOG_SVC_ERROR("RTSP: send queue init failed");
        /* streaming won't work, but the control server can still run */
    } else {
        osThreadAttr_t send_attr = {
            .name = "rtsp_send",
            .stack_size = RTSP_SEND_TASK_STACK_SIZE,
            .stack_mem = s_send_task_stack,
            .priority = RTSP_TASK_PRIORITY,
        };
        s_send_task_handle = osThreadNew(rtsp_send_task, NULL, &send_attr);
        if (!s_send_task_handle) {
            LOG_SVC_ERROR("RTSP: failed to create send task");
        }
    }

    LOG_SVC_INFO("RTSP service started on port %lu", (unsigned long)g_rtsp_ctx.listen_port);
    return AICAM_OK;
}

aicam_result_t rtsp_service_stop(void)
{
    if (!g_rtsp_ctx.running) {
        return AICAM_OK;
    }

    LOG_SVC_INFO("RTSP service stopping...");

    g_rtsp_ctx.running = AICAM_FALSE;

    /* Unsubscribe from video hub so rtsp_on_frame stops being called */
    if (g_rtsp_ctx.hub_subscribed && g_rtsp_ctx.hub_subscriber_id >= 0) {
        video_hub_unsubscribe(g_rtsp_ctx.hub_subscriber_id);
        g_rtsp_ctx.hub_subscriber_id = VIDEO_HUB_INVALID_SUBSCRIBER_ID;
        g_rtsp_ctx.hub_subscribed = AICAM_FALSE;
    }

    /* Mark all clients as not playing / not in use so rtsp_on_frame
     * (which may still be executing on the hub thread) stops sending
     * data to sockets we're about to close. */
    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        g_rtsp_ctx.clients[i].playing = AICAM_FALSE;
        g_rtsp_ctx.clients[i].in_use = AICAM_FALSE;
    }
    osMutexRelease(g_rtsp_ctx.mutex);

    /* Join the send task first (running=FALSE above ends its loop) and
     * release its queue buffers. */
    if (s_send_task_handle) {
        osDelay(150); /* pop timeout is 100 ms */
        s_send_task_handle = NULL;
    }
    send_queue_deinit();

    /* Wait for the server task to exit (up to 3 s).
     * The task closes all sockets itself after exiting the select loop,
     * so we must NOT close them here - that would be a double-close. */
    if (g_rtsp_ctx.server_task_handle) {
        uint32_t wait_start = osKernelGetTickCount();
        do {
            osDelay(100);
            osThreadState_t state = osThreadGetState(g_rtsp_ctx.server_task_handle);
            if (state == osThreadTerminated || state == osThreadInactive) {
                break;
            }
        } while ((osKernelGetTickCount() - wait_start) < 3000);

        osThreadTerminate(g_rtsp_ctx.server_task_handle);
        g_rtsp_ctx.server_task_handle = NULL;
    }

    /* Defensive: if the task was killed before it could close sockets,
     * close any that are still open.  The task sets each variable to -1
     * BEFORE calling close(), so if a variable is still >= 0 the task
     * never got to it. */
    if (g_rtsp_ctx.listen_socket >= 0) {
        LOG_SVC_WARN("RTSP stop: task left listen socket %d open, closing",
                     g_rtsp_ctx.listen_socket);
        close(g_rtsp_ctx.listen_socket);
        g_rtsp_ctx.listen_socket = -1;
    }

    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (g_rtsp_ctx.clients[i].rtp_socket >= 0) {
            close(g_rtsp_ctx.clients[i].rtp_socket);
            g_rtsp_ctx.clients[i].rtp_socket = -1;
        }
        if (g_rtsp_ctx.clients[i].tcp_socket >= 0) {
            close(g_rtsp_ctx.clients[i].tcp_socket);
            g_rtsp_ctx.clients[i].tcp_socket = -1;
        }
        g_rtsp_ctx.clients[i].session_id[0] = '\0';
    }
    osMutexRelease(g_rtsp_ctx.mutex);

    /* Give lwIP's TCP timer a chance to process any pending PCB cleanup
     * (PCBs freed by close() are removed on the next tcp_tmr cycle). */
    osDelay(200);

    g_rtsp_ctx.service_state = SERVICE_STATE_INITIALIZED;

    LOG_SVC_INFO("RTSP service stopped");
    return AICAM_OK;
}

aicam_result_t rtsp_service_deinit(void)
{
    if (g_rtsp_ctx.running) {
        rtsp_service_stop();
    }

    if (g_rtsp_ctx.mutex) {
        osMutexDelete(g_rtsp_ctx.mutex);
        g_rtsp_ctx.mutex = NULL;
    }

    g_rtsp_ctx.initialized = AICAM_FALSE;
    g_rtsp_ctx.service_state = SERVICE_STATE_UNINITIALIZED;
    return AICAM_OK;
}

service_state_t rtsp_service_get_state(void)
{
    return g_rtsp_ctx.service_state;
}

aicam_bool_t rtsp_service_is_initialized(void)
{
    return g_rtsp_ctx.initialized;
}

aicam_bool_t rtsp_service_is_running(void)
{
    return g_rtsp_ctx.running;
}

/* ==================== Client Management ==================== */

static void rtsp_release_client(rtsp_client_t *client)
{
    if (client->rtp_socket >= 0) {
        close(client->rtp_socket);
        client->rtp_socket = -1;
    }
    if (client->tcp_socket >= 0) {
        close(client->tcp_socket);
        client->tcp_socket = -1;
    }
    client->in_use = AICAM_FALSE;
    client->playing = AICAM_FALSE;
    client->authenticated = AICAM_FALSE;
    client->session_id[0] = '\0';
    client->nonce[0] = '\0';
    client->client_rtp_port = 0;
    client->interleaved = AICAM_FALSE;
    client->interleave_rtp_channel = 0;
    client->interleave_rtcp_channel = 0;
    client->send_errors = 0;
    client->stalled = AICAM_FALSE;
    client->client_rtcp_port = 0;
    client->server_rtp_port = 0;
    client->rtp_seq = 0;
    client->rtp_timestamp = 0;
}

static void rtsp_disconnect_client(rtsp_client_t *client)
{
    if (client->playing) {
        rtsp_service_client_stop_play(client);
    }

    LOG_SVC_INFO("RTSP: Client disconnected session=%s", client->session_id);
    rtsp_release_client(client);
}

uint32_t rtsp_service_get_client_count(void)
{
    uint32_t count = 0;
    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (g_rtsp_ctx.clients[i].in_use) count++;
    }
    osMutexRelease(g_rtsp_ctx.mutex);
    return count;
}

void rtsp_service_get_clients(rtsp_client_t *clients, uint32_t *count)
{
    if (!clients || !count) return;
    *count = 0;

    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (g_rtsp_ctx.clients[i].in_use) {
            memcpy(&clients[*count], &g_rtsp_ctx.clients[i], sizeof(rtsp_client_t));
            (*count)++;
        }
    }
    osMutexRelease(g_rtsp_ctx.mutex);
}

aicam_result_t rtsp_service_kick_client(const char *session_id)
{
    if (!session_id) return AICAM_ERROR_INVALID_PARAM;

    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
    rtsp_client_t *client = rtsp_find_client_by_session(session_id);
    if (client) {
        LOG_SVC_INFO("RTSP: Kicking client session=%s", session_id);
        if (client->tcp_socket >= 0) {
            close(client->tcp_socket);
            client->tcp_socket = -1;
        }
    }
    osMutexRelease(g_rtsp_ctx.mutex);
    return client ? AICAM_OK : AICAM_ERROR_NOT_FOUND;
}

static rtsp_client_t* rtsp_find_client_by_session(const char *session_id)
{
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (g_rtsp_ctx.clients[i].in_use &&
            strcmp(g_rtsp_ctx.clients[i].session_id, session_id) == 0) {
            return &g_rtsp_ctx.clients[i];
        }
    }
    return NULL;
}

/* ==================== Client Helpers ==================== */

aicam_result_t rtsp_service_allocate_rtp(rtsp_client_t *client)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        LOG_SVC_ERROR("RTSP: Failed to create RTP socket (errno=%d)", errno);
        return AICAM_ERROR;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_SVC_ERROR("RTSP: Failed to bind RTP socket (errno=%d)", errno);
        close(sock);
        return AICAM_ERROR;
    }

    socklen_t addr_len = sizeof(addr);
    getsockname(sock, (struct sockaddr *)&addr, &addr_len);

    client->rtp_socket = sock;
    client->server_rtp_port = ntohs(addr.sin_port);

    LOG_SVC_INFO("RTSP: RTP socket allocated: sock=%d server_port=%u",
                 sock, client->server_rtp_port);
    return AICAM_OK;
}

aicam_result_t rtsp_service_client_start_play(rtsp_client_t *client)
{
    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
    client->playing = AICAM_TRUE;
    client->waiting_for_keyframe = AICAM_TRUE;
    osMutexRelease(g_rtsp_ctx.mutex);
    LOG_SVC_INFO("RTSP: Client started playing session=%s (waiting for keyframe)",
                 client->session_id);
    return AICAM_OK;
}

aicam_result_t rtsp_service_client_stop_play(rtsp_client_t *client)
{
    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
    client->playing = AICAM_FALSE;
    osMutexRelease(g_rtsp_ctx.mutex);
    return AICAM_OK;
}

void rtsp_service_get_auth_config(rtsp_service_auth_config_t *cfg)
{
    if (cfg) {
        memcpy(cfg, &g_rtsp_ctx.auth_config, sizeof(rtsp_service_auth_config_t));
    }
}

void rtsp_service_set_client_nonce(rtsp_client_t *client, const char *nonce)
{
    if (client && nonce) {
        strncpy(client->nonce, nonce, sizeof(client->nonce) - 1);
        client->nonce[sizeof(client->nonce) - 1] = '\0';
    }
}

void rtsp_service_get_device_ip(char *ip_buf, uint32_t buf_size)
{
    if (!ip_buf) return;

    /* Prefer the listen socket's actual bound address from the first connected client.
     * Fall back to netif IP, then to a default. */
    if (g_rtsp_ctx.device_ip[0]) {
        strncpy(ip_buf, g_rtsp_ctx.device_ip, buf_size - 1);
        ip_buf[buf_size - 1] = '\0';
    } else {
        strncpy(ip_buf, "192.168.1.1", buf_size - 1);
        ip_buf[buf_size - 1] = '\0';
    }
}

void rtsp_service_get_client_local_ip(rtsp_client_t *client, char *ip_buf, uint32_t buf_size)
{
    if (!ip_buf || !client || client->tcp_socket < 0) {
        rtsp_service_get_device_ip(ip_buf, buf_size);
        return;
    }

    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(client->tcp_socket, (struct sockaddr *)&local_addr, &addr_len) == 0) {
        snprintf(ip_buf, buf_size, "%s", inet_ntoa(local_addr.sin_addr));
    } else {
        rtsp_service_get_device_ip(ip_buf, buf_size);
    }
}

void rtsp_service_get_cached_sps_pps(rtsp_sps_pps_t *sps_pps)
{
    if (!sps_pps) return;

    if (g_rtsp_ctx.sps_pps_valid) {
        sps_pps->sps_data = g_rtsp_ctx.sps_data;
        sps_pps->sps_size = g_rtsp_ctx.sps_size;
        sps_pps->pps_data = g_rtsp_ctx.pps_data;
        sps_pps->pps_size = g_rtsp_ctx.pps_size;
    } else {
        sps_pps->sps_data = NULL;
        sps_pps->sps_size = 0;
        sps_pps->pps_data = NULL;
        sps_pps->pps_size = 0;
    }
}

/* ==================== Video Hub Callbacks ==================== */

static void rtsp_on_sps_pps(const video_hub_sps_pps_t *sps_pps, void *user_data)
{
    (void)user_data;
    if (!sps_pps) return;

    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);

    if (sps_pps->sps_data && sps_pps->sps_size > 0 && sps_pps->sps_size <= sizeof(g_rtsp_ctx.sps_data)) {
        memcpy(g_rtsp_ctx.sps_data, sps_pps->sps_data, sps_pps->sps_size);
        g_rtsp_ctx.sps_size = sps_pps->sps_size;
    }

    if (sps_pps->pps_data && sps_pps->pps_size > 0 && sps_pps->pps_size <= sizeof(g_rtsp_ctx.pps_data)) {
        memcpy(g_rtsp_ctx.pps_data, sps_pps->pps_data, sps_pps->pps_size);
        g_rtsp_ctx.pps_size = sps_pps->pps_size;
    }

    g_rtsp_ctx.sps_pps_valid = AICAM_TRUE;
    LOG_SVC_INFO("RTSP: SPS/PPS cached: sps=%lu pps=%lu sps_hdr=%02X pps_hdr=%02X",
                 (unsigned long)g_rtsp_ctx.sps_size, (unsigned long)g_rtsp_ctx.pps_size,
                 g_rtsp_ctx.sps_size > 0 ? g_rtsp_ctx.sps_data[0] : 0,
                 g_rtsp_ctx.pps_size > 0 ? g_rtsp_ctx.pps_data[0] : 0);
    LOG_SVC_INFO("RTSP: SPS first 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                 g_rtsp_ctx.sps_size > 0 ? g_rtsp_ctx.sps_data[0] : 0,
                 g_rtsp_ctx.sps_size > 1 ? g_rtsp_ctx.sps_data[1] : 0,
                 g_rtsp_ctx.sps_size > 2 ? g_rtsp_ctx.sps_data[2] : 0,
                 g_rtsp_ctx.sps_size > 3 ? g_rtsp_ctx.sps_data[3] : 0,
                 g_rtsp_ctx.sps_size > 4 ? g_rtsp_ctx.sps_data[4] : 0,
                 g_rtsp_ctx.sps_size > 5 ? g_rtsp_ctx.sps_data[5] : 0,
                 g_rtsp_ctx.sps_size > 6 ? g_rtsp_ctx.sps_data[6] : 0,
                 g_rtsp_ctx.sps_size > 7 ? g_rtsp_ctx.sps_data[7] : 0);
    osMutexRelease(g_rtsp_ctx.mutex);
}

static aicam_result_t rtsp_on_frame(const video_hub_frame_t *frame, void *user_data)
{
    (void)user_data;
    if (!frame || !frame->data || frame->size == 0) {
        return AICAM_OK;
    }
    /* Encoder-thread context: enqueue only. A copy here costs ~50us for a
     * 60KB frame; everything socket-related happens in rtsp_send_task. */
    return send_queue_push(frame->data, frame->size, frame->is_keyframe);
}

/* Dedicated send task: owns ALL RTP sends (UDP + interleaved TCP) so a
 * stalled client can never stall the encoder pipeline. select()-guarded
 * writes with a bounded budget (see rtp_sendto); on failure the client
 * drops to keyframe-wait (recovery at the next IDR, <=1 s) and repeated
 * failures release the client entirely. */
static void rtsp_send_task(void *argument)
{
    (void)argument;
    LOG_SVC_INFO("RTSP: send task started");
    static uint32_t send_frame_count = 0;

    while (g_rtsp_ctx.running) {
        rtsp_frame_entry_t entry;
        if (send_queue_pop(&entry) != AICAM_OK) {
            continue;
        }

        /* Copy out of the ring BEFORE sending (see staging note above). */
        memcpy(s_send_staging, entry.data, entry.size);
        entry.data = s_send_staging;

        osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);

        /* Frames were dropped (queue full / slow consumers): every playing
         * client must resync at the next keyframe, or they decode a broken
         * reference chain and show green/smeared video. */
        if (s_send_q.dropped) {
            s_send_q.dropped = 0;
            for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
                if (g_rtsp_ctx.clients[i].playing) {
                    g_rtsp_ctx.clients[i].waiting_for_keyframe = AICAM_TRUE;
                }
            }
            LOG_SVC_WARN("RTSP: send queue overflow (total drops=%lu), clients resync at next IDR",
                         (unsigned long)s_send_q.drop_count);
        }

        for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
            rtsp_client_t *client = &g_rtsp_ctx.clients[i];
            if (!client->playing || !client->in_use) {
                continue;
            }
            if (client->waiting_for_keyframe && !entry.is_keyframe) {
                continue;
            }

            /* RTP timestamp from a per-client frame counter (90kHz / 30fps):
             * the source timestamp lacks sub-second resolution. */
            #define RTP_TS_PER_FRAME    3000
            uint32_t rtp_ts = client->rtp_frame_index * RTP_TS_PER_FRAME;
            client->rtp_frame_index++;

            aicam_result_t ret = AICAM_OK;
            if (entry.is_keyframe && g_rtsp_ctx.sps_pps_valid) {
                client->waiting_for_keyframe = AICAM_FALSE;
                ret = rtsp_rtp_send_single_nal(client,
                        g_rtsp_ctx.sps_data, g_rtsp_ctx.sps_size, rtp_ts, 0);
                if (ret == AICAM_OK) {
                    ret = rtsp_rtp_send_single_nal(client,
                            g_rtsp_ctx.pps_data, g_rtsp_ctx.pps_size, rtp_ts, 0);
                }
            }
            if (ret == AICAM_OK) {
                ret = rtsp_rtp_send_frame(client, entry.data, entry.size,
                                          entry.is_keyframe, rtp_ts);
            }

            if (ret != AICAM_OK) {
                client->send_errors++;
                client->waiting_for_keyframe = AICAM_TRUE;
                if (client->send_errors >= RTSP_SEND_ERROR_LIMIT) {
                    /* NEVER close the socket from here: the server task
                     * still selects on it; a closed+reused fd makes it read
                     * foreign connections (mongoose HTTP responses showed
                     * up as "Unknown method" requests). Mark it and let the
                     * server task - the socket owner - do the release. */
                    LOG_SVC_WARN("RTSP: client %d stalled (%d send errors), deferred release",
                                 i, client->send_errors);
                    client->playing = AICAM_FALSE;
                    client->stalled = AICAM_TRUE;
                }
            } else {
                client->send_errors = 0;
    client->stalled = AICAM_FALSE;
            }
        }

        send_frame_count++;
        if (send_frame_count <= 10 || (send_frame_count % 100) == 0) {
            LOG_SVC_INFO("RTSP: send task #%lu (size=%lu key=%d drops=%lu)",
                         (unsigned long)send_frame_count,
                         (unsigned long)entry.size, entry.is_keyframe,
                         (unsigned long)s_send_q.drop_count);
        }

        osMutexRelease(g_rtsp_ctx.mutex);
    }
    LOG_SVC_INFO("RTSP: send task exited");
}

/* ==================== Accept & Client I/O ==================== */

static void rtsp_accept_client(void)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_sock = accept(g_rtsp_ctx.listen_socket,
                             (struct sockaddr *)&client_addr, &addr_len);
    if (client_sock < 0) {
        return;
    }

    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);

    rtsp_client_t *client = NULL;
    int slot = -1;
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (!g_rtsp_ctx.clients[i].in_use) {
            client = &g_rtsp_ctx.clients[i];
            slot = i;
            break;
        }
    }

    if (!client) {
        osMutexRelease(g_rtsp_ctx.mutex);
        LOG_SVC_WARN("RTSP: No free client slots, rejecting connection");
        close(client_sock);
        return;
    }

    memset(client, 0, sizeof(rtsp_client_t));
    client->tcp_socket = client_sock;
    client->client_addr = client_addr;
    client->in_use = AICAM_TRUE;
    client->ssrc = (uint32_t)(rand() + slot * 10000);
    client->connected_time = osKernelGetTickCount();
    client->rtp_socket = -1;

    /* Get local IP from client socket (the IP the client actually connected to) */
    struct sockaddr_in local_addr;
    socklen_t local_len = sizeof(local_addr);
    if (getsockname(client_sock, (struct sockaddr *)&local_addr, &local_len) == 0) {
        snprintf(g_rtsp_ctx.device_ip, sizeof(g_rtsp_ctx.device_ip),
                 "%s", inet_ntoa(local_addr.sin_addr));
    }

    osMutexRelease(g_rtsp_ctx.mutex);

    LOG_SVC_INFO("RTSP: Client connected from %s:%u (slot %d, local_ip=%s)",
                 inet_ntoa(client_addr.sin_addr),
                 ntohs(client_addr.sin_port), slot,
                 g_rtsp_ctx.device_ip);
}

static void rtsp_handle_client_data(rtsp_client_t *client)
{
    /* Defensive: client may have been disconnected between the select snapshot
     * and this call (kick/stop from another thread).  The pointer itself is
     * always valid (static array), but sockets may be closed. */
    if (!client->in_use || client->tcp_socket < 0) return;

    /* Static to avoid ~2KB stack allocation (RTSP task is single-threaded) */
    static char s_recv_buf[RTSP_RECV_BUF_SIZE];
    memset(s_recv_buf, 0, sizeof(s_recv_buf));

    uint32_t total = 0;

    /* Read available data */
    while (total < sizeof(s_recv_buf) - 1) {
        int n = recv(client->tcp_socket, s_recv_buf + total, sizeof(s_recv_buf) - 1 - total,
                     MSG_DONTWAIT);
        if (n <= 0) {
            if (n == 0 || total == 0) {
                /* Connection closed or no data - disconnect */
                LOG_SVC_INFO("RTSP: Client recv=%d total=%lu, disconnecting session=%s",
                             n, (unsigned long)total, client->session_id);
                osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
                rtsp_disconnect_client(client);
                osMutexRelease(g_rtsp_ctx.mutex);
                return;
            }
            break;
        }
        total += n;
        s_recv_buf[total] = '\0';

        /* Check if we have a complete RTSP request (end of headers) */
        if (strstr(s_recv_buf, "\r\n\r\n")) {
            break;
        }
    }

    if (total == 0) return;

    /* Log the first line of the request (method + URI) */
    {
        char first_line[128] = {0};
        const char *eol = strstr(s_recv_buf, "\r\n");
        if (eol) {
            uint32_t len = (eol - s_recv_buf) < (sizeof(first_line) - 1) ? (eol - s_recv_buf) : (sizeof(first_line) - 1);
            memcpy(first_line, s_recv_buf, len);
        }
        LOG_SVC_INFO("RTSP RECV [%lu bytes]: %s", (unsigned long)total, first_line);
    }

    /* Handle the RTSP request */
    aicam_result_t ret = rtsp_handle_request(client, s_recv_buf, total);
    if (ret != AICAM_OK) {
        LOG_SVC_WARN("RTSP: Request handling failed ret=%d session=%s", ret, client->session_id);
    }

    /* Check for pipelined requests - data remaining after \r\n\r\n */
    {
        const char *end = strstr(s_recv_buf, "\r\n\r\n");
        if (end) {
            uint32_t consumed = (end - s_recv_buf) + 4;
            if (consumed < total) {
                LOG_SVC_INFO("RTSP: Pipelined data detected (%lu bytes remaining after first request)",
                             (unsigned long)(total - consumed));
                /* Log the start of the next request */
                char next_line[128] = {0};
                const char *remaining = s_recv_buf + consumed;
                const char *next_eol = strstr(remaining, "\r\n");
                if (next_eol) {
                    uint32_t nlen = (next_eol - remaining) < (sizeof(next_line) - 1) ?
                                    (next_eol - remaining) : (sizeof(next_line) - 1);
                    memcpy(next_line, remaining, nlen);
                    LOG_SVC_INFO("RTSP: Next pipelined request: %s", next_line);
                }
            }
        }
    }
}

/* ==================== Server Task (select-based multiplexing) ==================== */

static void rtsp_server_task(void *arg)
{
    (void)arg;
    LOG_SVC_INFO("RTSP server task started (select-based, port=%lu)",
                 (unsigned long)g_rtsp_ctx.listen_port);

    uint32_t loop_count = 0;

    while (g_rtsp_ctx.running && g_rtsp_ctx.listen_socket >= 0) {
        fd_set read_fds;
        FD_ZERO(&read_fds);

        int listen_fd = g_rtsp_ctx.listen_socket;
        FD_SET(listen_fd, &read_fds);
        int max_fd = listen_fd;

        osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
        for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
            /* The send task defers stalled clients here: this task owns the
             * sockets, so it is the only context allowed to close them. */
            if (g_rtsp_ctx.clients[i].in_use && g_rtsp_ctx.clients[i].stalled) {
                LOG_SVC_WARN("RTSP: releasing stalled client %d (server task)", i);
                rtsp_release_client(&g_rtsp_ctx.clients[i]);
            }
            int fd = g_rtsp_ctx.clients[i].tcp_socket;
            if (g_rtsp_ctx.clients[i].in_use && fd >= 0) {
                FD_SET(fd, &read_fds);
                if (fd > max_fd) max_fd = fd;
            }
        }
        osMutexRelease(g_rtsp_ctx.mutex);

        struct timeval tv = {
            .tv_sec = RTSP_SELECT_TIMEOUT_MS / 1000,
            .tv_usec = (RTSP_SELECT_TIMEOUT_MS % 1000) * 1000
        };

        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (!g_rtsp_ctx.running) break;
            LOG_SVC_WARN("RTSP: select() error, continuing");
            continue;
        }
        if (ret == 0) {
            loop_count++;
            /* Log heartbeat every 10 seconds (10 iterations with 1s timeout) */
            if ((loop_count % 10) == 0) {
                int client_count = 0;
                osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
                for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
                    if (g_rtsp_ctx.clients[i].in_use) client_count++;
                }
                osMutexRelease(g_rtsp_ctx.mutex);
                LOG_SVC_INFO("RTSP: select loop alive, clients=%d, listen_fd=%d",
                             client_count, listen_fd);
            }
            continue;
        }

        loop_count = 0; /* reset on activity */

        /* New connection? */
        if (FD_ISSET(listen_fd, &read_fds)) {
            rtsp_accept_client();
        }

        /* Client data? Take a snapshot of in-use clients under lock,
         * then handle each one. Disconnected clients are cleaned up
         * inside rtsp_handle_client_data. */
        rtsp_client_t *to_check[RTSP_MAX_CLIENTS];
        int to_check_count = 0;

        osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
        for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
            if (g_rtsp_ctx.clients[i].in_use &&
                g_rtsp_ctx.clients[i].tcp_socket >= 0 &&
                FD_ISSET(g_rtsp_ctx.clients[i].tcp_socket, &read_fds)) {
                to_check[to_check_count++] = &g_rtsp_ctx.clients[i];
            }
        }
        osMutexRelease(g_rtsp_ctx.mutex);

        for (int i = 0; i < to_check_count; i++) {
            rtsp_handle_client_data(to_check[i]);
        }
    }

    /* Task is exiting - close all sockets HERE, from the same thread that
     * called select().  lwIP's select_waiting is only decremented inside
     * select() itself, so close() from ANY other thread will leave
     * select_waiting != 0 on the socket slot.  When alloc_socket() later
     * reuses that slot it asserts "select_waiting == 0".  Closing from this
     * thread guarantees select() has fully returned before we close.
     *
     * Set the variable to -1 BEFORE calling close().  If the stop function
     * terminates this task mid-cleanup, the stop function sees -1 and
     * doesn't attempt a double-close on the same fd. */
    {
        int fd = g_rtsp_ctx.listen_socket;
        g_rtsp_ctx.listen_socket = -1;
        if (fd >= 0) {
            LOG_SVC_INFO("RTSP server task: closing listen socket %d", fd);
            close(fd);
        }
    }

    osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (g_rtsp_ctx.clients[i].in_use) {
            g_rtsp_ctx.clients[i].playing = AICAM_FALSE;
            /* Take local copies, set to -1, then close - same pattern as
             * listen socket above, prevents double-close if task is killed. */
            int tcp_fd = g_rtsp_ctx.clients[i].tcp_socket;
            int rtp_fd = g_rtsp_ctx.clients[i].rtp_socket;
            g_rtsp_ctx.clients[i].tcp_socket = -1;
            g_rtsp_ctx.clients[i].rtp_socket = -1;
            g_rtsp_ctx.clients[i].in_use = AICAM_FALSE;
            g_rtsp_ctx.clients[i].session_id[0] = '\0';
            osMutexRelease(g_rtsp_ctx.mutex);
            if (rtp_fd >= 0) close(rtp_fd);
            if (tcp_fd >= 0) close(tcp_fd);
            osMutexAcquire(g_rtsp_ctx.mutex, osWaitForever);
        }
    }
    osMutexRelease(g_rtsp_ctx.mutex);

    LOG_SVC_INFO("RTSP server task exited");
}
