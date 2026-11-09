import request from '../request';

/** Dual-model engine (Model A -> crop -> Model B) configuration, exactly as
 * the firmware API exposes it (see Custom/Core/Video/dual_model_config.h).
 * Thresholds are x100 integers; crop_expand_pm is permille per side. */
export interface DualModelConfig {
  enabled: boolean;
  target_conf_threshold: number;
  target_class: string;
  crop_expand_pm: number;
  crop_min_size: number;
  max_targets: number;
  interval_ms: number;
  pad_value: number;
}

export interface DualModelStatus {
  enabled: boolean;
  model_b_loaded: boolean;
  model_b_name: string;
  model_b_input_width: number;
  model_b_input_height: number;
  model_b_postprocess: string;
  model_b_addr: number;
  passes: number;
  inferences: number;
  errors: number;
  last_pass_ms: number;
  last_targets: number;
  avg_infer_ms: number;
  skipped_throttle: number;
  disabled_by_errors: boolean;
}

/** Face recognition demo configuration (see face_recognition_config.h) */
export interface FaceRecognitionConfig {
  enabled: boolean;
  match_threshold: number;
  confirm_votes: number;
  track_iou_threshold: number;
  track_max_missed: number;
  face_hold_ms: number;
  enroll_samples: number;
  enroll_timeout_ms: number;
  enroll_min_similarity: number;
  pose_max_yaw: number;
  pose_max_roll: number;
  show_box: boolean;
  show_name: boolean;
  show_similarity: boolean;
}

export type FrTrackStatus = 'identifying' | 'known' | 'unknown';

export interface FrTrackInfo {
  track_id: number;
  x: number;
  y: number;
  w: number;
  h: number;
  detection_confidence: number;
  status: FrTrackStatus;
  name: string;
  db_id: number;
  similarity: number;
  frontal?: boolean;
  pose_score?: number;
}

export interface FrResults {
  count: number;
  tracks: FrTrackInfo[];
}

export interface FrStats {
  enabled: boolean;
  confirmed_tracks: number;
  total_tracks: number;
  embed_count: number;
  recognized_count: number;
  unknown_count: number;
  db_count: number;
  last_embed_ms: number;
  enroll?: {
    state: FrEnrollState;
    samples_collected: number;
    samples_needed: number;
    new_id: number;
    capture_armed?: number;
  };
  model_b?: {
    loaded: boolean;
    name: string;
    input_width: number;
    input_height: number;
    postprocess: string;
    avg_infer_ms: number;
    disabled_by_errors: boolean;
  };
}

export type FrEnrollState =
  | 'idle'
  | 'collecting'
  | 'paused'
  | 'done'
  | 'failed_no_face'
  | 'failed_inconsistent'
  | 'failed_db';

export interface FrEnrollStatus {
  state: FrEnrollState;
  samples_collected: number;
  samples_needed: number;
  new_id: number;
  capture_armed?: number;
}

export interface FrFaceEntry {
  id: number;
  name: string;
  dim: number;
  created_at: number;
}

export interface FrFaceList {
  count: number;
  faces: FrFaceEntry[];
}

const faceRecognitionApis = {
  /* ---------- demo config ---------- */
  getConfigReq: () => request.get('/api/v1/face-recognition/config'),
  setConfigReq: (data: Partial<FaceRecognitionConfig>) => request.post('/api/v1/face-recognition/config', data),

  /* ---------- live state ---------- */
  getStatusReq: () => request.get('/api/v1/face-recognition/status'),
  getResultsReq: () => request.get('/api/v1/face-recognition/results'),
  resetReq: () => request.post('/api/v1/face-recognition/reset'),

  /* ---------- face database ---------- */
  getFacesReq: () => request.get('/api/v1/face-recognition/faces'),
  enrollReq: (name: string) => request.post('/api/v1/face-recognition/faces/enroll', { name }),
  enrollCaptureReq: () => request.post('/api/v1/face-recognition/faces/enroll/capture'),
  enrollFinishReq: () => request.post('/api/v1/face-recognition/faces/enroll/finish'),
  getEnrollStatusReq: () => request.get('/api/v1/face-recognition/faces/enroll/status'),
  cancelEnrollReq: () => request.post('/api/v1/face-recognition/faces/enroll/cancel'),
  deleteFaceReq: (id: number) => request.post('/api/v1/face-recognition/faces/delete', { id }),
  renameFaceReq: (id: number, name: string) => request.post('/api/v1/face-recognition/faces/rename', { id, name }),
  clearFacesReq: () => request.post('/api/v1/face-recognition/faces/clear'),

  /* ---------- dual-model engine (Model B) ---------- */
  getDualModelConfigReq: () => request.get('/api/v1/dual-model/config'),
  setDualModelConfigReq: (data: Partial<DualModelConfig>) => request.post('/api/v1/dual-model/config', data),
  getDualModelStatusReq: () => request.get('/api/v1/dual-model/status'),
  reloadModelBReq: () => request.post('/api/v1/dual-model/reload'),
};

export default faceRecognitionApis;
