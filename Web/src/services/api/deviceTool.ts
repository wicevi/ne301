import request from '../request';

export interface WorkModeSwitchReq {
  mode: 'image' | 'video_stream';
}

export interface AiParams {
  nms_threshold: number;
  confidence_threshold: number;
}
export interface VideoStreamPushReq {
  enabled: boolean;
  server_url?: string;
}
// export interface ImageTriggerReq {
//     io_trigger?: boolean,
//     pir_trigger?: boolean,
//     timer_trigger?: boolean
// }

export interface PhotoCaptureReq {
  enable_ai: boolean;
  chunk_size: number;
  store_to_sd: boolean;
}

export interface RtmpConfigReq {
  enable?: boolean;
  url?: string;
  stream_key?: string;
}
export interface RtmpStartReq {
  url?: string;
  stream_key?: string;
}

export type RtspAuthMode = 'none' | 'digest';

export interface RtspConfigReq {
  enabled: boolean;
  port: number;
  auth_mode: RtspAuthMode;
  username: string;
  password: string;
}

export interface RtspStatus {
  running: boolean;
  stream_url: string;
}

export interface RtspClient {
  id: string;
  ip: string;
  connected_duration: string;
}

export interface PirConfigReq {
  pir_trigger: {
    enable: boolean;
    device_type?: 'pir' | 'io'; // What drives the wake input: PIR chip or plain IO signal
    trigger_type:
      | 'rising_edge'
      | 'falling_edge'
      | 'high_level'
      | 'low_level'
      | 'both_edges';
    sensitivity_level: number; // 10-255 (register value; UI maps it to 1-100)
    ignore_time_s: number; // 0-15 (register; seconds = 0.5 + 0.5*value)
    pulse_count: number; // 1-4
    window_time_s: number; // 0-3 (register; seconds = 2 + 2*value)
    disable_in_preview: boolean; // Disable PIR capture during preview (default: false)
  };
  timer_trigger?: {
    enable: boolean;
    capture_mode: string;
    interval_sec: number;
    time_node_count: number;
    time_node: string[];
    weekdays: number[];
    interval_mode: 'normal' | 'scheduled';
    start_time: string;
    end_time?: string; // Scheduled mode daily window end "HH:MM"; 00:00 = ends at midnight (full day is start T with end T-1min); absent = old firmware, falls back to a full-day display
    anchor?: string; // Normal interval mode daily grid anchor "HH:MM" (time-of-day; device stamps current time when unset)
    next_capture_at?: number; // Read-only: next capture node, unix seconds
  };
  remote_trigger?: {
    enable: boolean;
  };
}

export interface TriggerConfigRes {
  pir_trigger?: PirConfigReq['pir_trigger'] & { pir_apply_failed?: boolean };
  timer_trigger?: PirConfigReq['timer_trigger'];
  remote_trigger?: { enable: boolean };
  pir_apply_failed?: boolean;
}

export type StreamResolution = 'pipe2' | '720p' | '1080p';

export interface StreamConfigReq {
  resolution: StreamResolution;
  bitrate_kbps: number; // 256..8192
  gop_sec: 1 | 2 | 4; // keyframe interval, seconds
}

// Runtime capture parameters (apply to captures taken while the device is
// running - NOT the sleep-wake quick path). Resolution follows the stream
// config (see the confirm dialog on the deviceTool page).
export interface RuntimeCaptureConfig {
  runtime_capture_skip_frames: number; // 0..30
  runtime_capture_jpeg_quality: number; // 1..100
  /** AI master switch for captures: off = capture without AI inference */
  ai_capture_enable: boolean;
}
const deviceTool = {
  // Whether to enable video stream
  startVideoStreamReq: () => request.post('api/v1/preview/start'),
  stopVideoStreamReq: () => request.post('api/v1/preview/stop'),

  // Get work mode status
  getWorkModeStatusReq: () => request.get('/api/v1/work-mode/status'),
  // Switch work mode
  switchWorkModeReq: (data: WorkModeSwitchReq) => request.post('/api/v1/work-mode/switch', data),

  // Configure video stream push
  configVideoStreamPushReq: (data: VideoStreamPushReq) => request.post('/api/v1/work-mode/video-stream/config', data),

  // Configure image mode trigger
  configTriggerConfigReq: (data: PirConfigReq) => request.post('/api/v1/work-mode/triggers', data),

  // Partial-update endpoints: timer / remote only, skip the (slow, unrelated)
  // chip-level PIR write that the combined endpoint always performs.
  configTimerTriggerReq: (timer_trigger: NonNullable<PirConfigReq['timer_trigger']>) => request.post('/api/v1/work-mode/timer-trigger', { timer_trigger }),
  configRemoteTriggerReq: (enable: boolean) => request.post('/api/v1/work-mode/remote-trigger', { enable }),

  getTriggerConfigReq: () => request.get('/api/v1/work-mode/triggers'),

  // Live video stream parameters (resolution / bitrate / gop)
  getStreamConfigReq: () => request.get('/api/v1/device/stream/config'),
  setStreamConfigReq: (data: StreamConfigReq) => request.post('/api/v1/device/stream/config', data),

  // Runtime capture parameters (skip frames / jpeg quality). GET returns the
  // full camera config; the fields live under image_config. POST is a nested
  // partial update.
  getRuntimeCaptureReq: async (): Promise<{ data: { image_config: RuntimeCaptureConfig } }> => request.get('/api/v1/device/camera/config'),
  setRuntimeCaptureReq: (data: Partial<RuntimeCaptureConfig>) => request.post('/api/v1/device/camera/config', { image_config: data }),

  // Switch power mode
  switchPowerModeReq: (data: { mode: string }) => request.post('api/v1/power-mode/switch', data),
  getPowerModeReq: () => request.get('api/v1/power-mode/status'),
  // -----------------

  // Get AI status
  getAiStatusReq: () => request.get('/api/v1/ai/status'),
  toggleAiReq: (data: { ai_enabled: boolean }) => request.post('/api/v1/ai/toggle', data),

  // AI parameters
  getAiParamsReq: () => request.get('/api/v1/ai/params'),
  setAiParamsReq: (data: AiParams) => request.post('/api/v1/ai/params', data),

  // Photo capture
  photoCaptureReq: (data: PhotoCaptureReq) => request.post('/api/v1/device/capture', data),

  // RTMP
  getRtmpConfigReq: () => request.get('/api/v1/apps/rtmp/config'),
  setRtmpConfigReq: (data: RtmpConfigReq) => request.post('/api/v1/apps/rtmp/config', data),
  startRtmpReq: (data: RtmpStartReq) => request.post('/api/v1/apps/rtmp/start', data),
  stopRtmpReq: () => request.post('/api/v1/apps/rtmp/stop'),
  getRtmpStatusReq: () => request.get('/api/v1/apps/rtmp/status'),

  // RTSP
  getRtspConfigReq: () => request.get('/api/v1/apps/rtsp/config'),
  setRtspConfigReq: (data: RtspConfigReq) => request.put('/api/v1/apps/rtsp/config', data),
  getRtspStatusReq: () => request.get('/api/v1/apps/rtsp/status'),
  getRtspClientsReq: () => request.get('/api/v1/apps/rtsp/clients'),
  kickRtspClientReq: (id: string) => request.delete(`/api/v1/apps/rtsp/clients/${id}`),

  getStreamTabPrefReq: () => (request.get('/api/v1/device/preference/stream_tab')),
  setStreamTabPrefReq: (data: { stream_tab: string }) => (request.post('/api/v1/device/preference/stream_tab', data)),

  // // PIR
  // getPirConfigReq: () => request.get('/api/v1/work-mode/triggers'),
  // setPirConfigReq: (data: PirConfigReq) => request.post('/api/v1/work-mode/triggers', data),
};

export default deviceTool;
