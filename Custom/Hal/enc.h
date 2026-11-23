#ifndef ENC_H
#define ENC_H

#include <stddef.h>
#include <stdint.h>
#include "cmsis_os2.h"
#include "dev_manager.h"
#include "aicam_error.h"
#include "h264encapi.h"
#include "jpegencapi.h"

#define USE_H264_VENC  1

#define VENC_ALLOCATOR_SIZE (4 * 1024 * 1024)

#define VENC_DEFAULT_WIDTH 1280
#define VENC_DEFAULT_HEIGHT 720
#define VENC_DEFAULT_FPS 30
#define VENC_DEFAULT_INPUT_TYPE H264ENC_RGB565
#define VENC_DEFAULT_BPP 2
#define VENC_DEFAULT_RATE_CTRL_QP 25

/* Encoder output buffer budget, derived from the stream geometry AND the
 * configured bitrate: a large I-frame (QP floor / pre-rate-control-convergence)
 * needs roughly 1 byte per 3 pixels - 720p ~= 307 KB, 1080p ~= 691 KB - and at
 * high bitrates the rate controller lets a 1-second-GOP I-frame approach the
 * whole per-second byte budget (5000 kbps ~= 625 KB even at 720p, which the
 * geometry-only estimate under-budgeted and dropped the encoder to ~20 fps
 * with OUTPUT_BUFFER_OVERFLOW). The budget is therefore
 *   max(width*height/3, bitrate_kbps*1000/8) clamped to [MIN, MAX].
 * The backing array lives in PSRAM at the worst-case size (1 MB == the UI's
 * 8192 kbps ceiling). */
#define VENC_OUT_BUFFER_MIN  (256u * 1024u)
#define VENC_OUT_BUFFER_MAX  (1024u * 1024u)
uint32_t venc_out_buffer_size_for(uint32_t width, uint32_t height, uint32_t bitrate_kbps);

#define ENC_FRAME_HEADER_SIZE (1*64)
enum {
    VENC_RATE_CTRL_QP_CONSTANT,
    VENC_RATE_CTRL_VBR,
};

typedef enum {
    ENC_CMD_GET_STATE        = ENC_CMD_BASE,
    ENC_CMD_SET_PARAM,
    ENC_CMD_GET_PARAM,
    ENC_CMD_INPUT_BUFFER,
    ENC_CMD_OUTPUT_BUFFER,
    ENC_CMD_OUTPUT_FRAME,
} ENC_CMD_E;

typedef enum {
    ENC_STOP = 0,
    ENC_IDLE,
    ENC_PROCESSING,
    ENC_COMPLETE,
} ENC_STATE_E;

typedef struct {
    H264EncOut frame_info;
    uint8_t *frame_buffer;  //base buffer pointer.header + data
    uint32_t header_size;
    uint32_t data_size;//venc size
} enc_out_frame_t;
typedef struct {
    int width;
    int height;
    int fps;
    JpegEncFrameType input_type;
    int bpp;
    int rate_ctrl_mode;
    int rate_ctrl_dq;
    int bitrate_kbps;   /* >0: VBR target bitrate override; 0: derive from w*h*fps */
    int gop_len;        /* >0: IDR interval in frames override; 0: fps-1 (1 second) */
} enc_param_t;
typedef struct {
    bool is_init;
    ENC_STATE_E state;
    device_t *dev;
    osMutexId_t mtx_id;
    osSemaphoreId_t sem_id;
    osThreadId_t enc_processId;
    osMutexId_t state_mtx;   // protect state and pointer
    osMutexId_t hw_mtx;      // protect hardware/codec long time operation
    osSemaphoreId_t sem_work; // task work notification (producer->consumer)
    osEventFlagsId_t evt_flags;
    enc_param_t params;
    uint32_t out_buffer_size;  // adaptive budget for the current params (see VENC_OUT_BUFFER_*)
    uint8_t *in_buffer;
    enc_out_frame_t out_frame;
    int is_intra_force;
    int startup_failures;    /* consecutive output-buffer overflows since the last success */
    int qp_floor;            /* QP floor in force; 0 = configured quality */
} enc_t;

int enc_register(void);
int enc_unregister(void);
#endif