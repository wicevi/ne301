import { useEffect, useState, useRef, useCallback } from 'react';
import { useLingui } from '@lingui/react';
import { Label } from '@/components/ui/label';
import { Separator } from '@/components/ui/separator';
import { Switch } from '@/components/ui/switch';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import {
  Table, TableBody, TableCell, TableHead, TableHeader, TableRow,
} from '@/components/ui/table';
import { Progress } from '@/components/ui/progress';
import H264Player from '@/lib/MSE/h264Player';
import Loading from '@/components/loading';
import deviceTool from '@/services/api/deviceTool';
import faceRecognition, {
  type FaceRecognitionConfig,
  type DualModelConfig,
  type FrStats,
  type FrResults,
  type FrTrackInfo,
  type FrFaceList,
  type FrEnrollState,
} from '@/services/api/faceRecognition';
import { getWebSocketUrl } from '@/utils';
import { previewStreamStart, previewStreamStop } from '@/utils/previewStream';
import { toast } from 'sonner';

/* Sections below the video share its width */
const SECTION_MAX_W = { maxWidth: 'calc(62vh * 16 / 9)' };

const TRACK_STATUS_CLASS: Record<FrTrackInfo['status'], string> = {
  identifying: 'bg-sky-600/80 text-white',
  known: 'bg-green-600/80 text-white',
  unknown: 'bg-gray-500/80 text-white',
};

const pct = (v: number) => `${Math.round(v * 100)}%`;

const ENROLL_FAIL_TOAST: Record<string, string> = {
  failed_no_face: 'sys.face_recognition.enroll_failed_no_face',
  failed_inconsistent: 'sys.face_recognition.enroll_failed_inconsistent',
  failed_db: 'sys.face_recognition.enroll_failed_db',
};

export default function FaceRecognition() {
  const { i18n } = useLingui();
  const { toggleAiReq } = deviceTool;

  /* ---------- video stream ---------- */
  const playerRef = useRef<HTMLVideoElement>(null);
  const playerRenderRef = useRef<H264Player | null>(null);
  const [playerLoading, setPlayerLoading] = useState(false);
  const [playerEpoch] = useState(0);

  const startStream = useCallback(async () => {
    try {
      setPlayerLoading(true);
      await previewStreamStart();
      if (!playerRef.current) return;
      const player = new H264Player(() => {});
      playerRenderRef.current = player;
      player.initPlayer(playerRef.current);
      player.start(getWebSocketUrl());
    } catch (error) {
      console.error(error);
    } finally {
      setPlayerLoading(false);
    }
  }, []);

  /* ---------- stream lifecycle: AI on + preview ---------- */
  useEffect(() => {
    let cancelled = false;
    (async () => {
      // The dual-model engine consumes the ACTIVE model's results; make sure
      // the AI pipeline is running before the preview comes up.
      try {
        await toggleAiReq({ ai_enabled: true });
      } catch (error) {
        console.error(error);
      }
      if (!cancelled) await startStream();
    })();
    return () => {
      cancelled = true;
      playerRenderRef.current?.destroy();
      playerRenderRef.current = null;
      previewStreamStop();
    };
  }, []);

  /* ---------- state ---------- */
  const [stats, setStats] = useState<FrStats | null>(null);
  const [results, setResults] = useState<FrResults | null>(null);
  const [faces, setFaces] = useState<FrFaceList | null>(null);
  const [config, setConfig] = useState<FaceRecognitionConfig | null>(null);
  const [paramDraft, setParamDraft] = useState<Record<string, number | boolean | string>>({});
  const [savingParams, setSavingParams] = useState(false);

  /* ---------- enrollment ---------- */
  const [enrollName, setEnrollName] = useState('');
  const [enrollState, setEnrollState] = useState<FrEnrollState>('idle');
  const [enrollProgress, setEnrollProgress] = useState({ collected: 0, needed: 0 });
  const [captureArmed, setCaptureArmed] = useState(false);
  const lastEnrollStateRef = useRef<FrEnrollState>('idle');

  /* ---------- face db ---------- */
  const [renamingId, setRenamingId] = useState<number | null>(null);
  const [renameDraft, setRenameDraft] = useState('');
  const [clearDialogOpen, setClearDialogOpen] = useState(false);

  /* ---------- data loading ---------- */
  const loadConfig = useCallback(async () => {
    try {
      const [c, d] = await Promise.all([
        faceRecognition.getConfigReq(),
        faceRecognition.getDualModelConfigReq(),
      ]);
      const fr = c.data as FaceRecognitionConfig;
      const dm = d.data as DualModelConfig;
      setConfig(fr);
      /* Model B fields are prefixed dm_* so the demo's own `enabled` (and
       * other same-named fields) never collide in the draft */
      setParamDraft({
        ...fr,
        dm_enabled: dm.enabled,
        dm_target_conf_threshold: dm.target_conf_threshold,
        dm_crop_expand_pm: dm.crop_expand_pm,
        dm_min: dm.crop_min_size,
        dm_max_targets: dm.max_targets,
        dm_interval_ms: dm.interval_ms,
        dm_pad_value: dm.pad_value,
      });
    } catch (error) {
      console.error(error);
    }
  }, []);

  const loadFaces = useCallback(async () => {
    try {
      const res = await faceRecognition.getFacesReq();
      setFaces(res.data as FrFaceList);
    } catch (error) {
      console.error(error);
    }
  }, []);

  useEffect(() => {
    loadConfig();
    loadFaces();
  }, [loadConfig, loadFaces]);

  /* ---------- 1 s poll: status + results (enrollment rides along) ---------- */
  useEffect(() => {
    let stopped = false;
    const poll = async () => {
      try {
        const [s, r] = await Promise.all([
          faceRecognition.getStatusReq(),
          faceRecognition.getResultsReq(),
        ]);
        if (stopped) return;
        const st = s.data as FrStats;
        setStats(st);
        setResults(r.data as FrResults);

        // Enrollment transitions -> toast once on change
        const en = st.enroll;
        if (en) {
          setEnrollState(en.state);
          setEnrollProgress({ collected: en.samples_collected, needed: en.samples_needed });
          setCaptureArmed(Boolean(en.capture_armed));
          const prev = lastEnrollStateRef.current;
          if (en.state !== prev) {
            if (prev === 'collecting' && en.state === 'done') {
              toast.success(i18n._('sys.face_recognition.enroll_done'));
              loadFaces();
            } else if (prev === 'collecting' && en.state === 'paused') {
              toast.warning(i18n._('sys.face_recognition.enroll_paused_hint'));
            } else if (prev === 'collecting' && ENROLL_FAIL_TOAST[en.state]) {
              toast.error(i18n._(ENROLL_FAIL_TOAST[en.state]));
            }
            lastEnrollStateRef.current = en.state;
          }
        }
      } catch {
        /* transient network hiccups are retried by the request layer */
      }
    };
    poll();
    const timer = setInterval(poll, 1000);
    return () => {
      stopped = true;
      clearInterval(timer);
    };
  }, []);

  /* ---------- config actions ---------- */
  const patchConfig = async (
    patch: Record<string, unknown>,
    successKey?: string,
  ) => {
    try {
      /* Split the flat patch into the two firmware endpoints */
      const frFields: Record<string, keyof FaceRecognitionConfig> = {
        enabled: 'enabled',
        match_threshold: 'match_threshold',
        confirm_votes: 'confirm_votes',
        track_iou_threshold: 'track_iou_threshold',
        track_max_missed: 'track_max_missed',
        face_hold_ms: 'face_hold_ms',
        enroll_samples: 'enroll_samples',
        enroll_timeout_ms: 'enroll_timeout_ms',
        enroll_min_similarity: 'enroll_min_similarity',
        show_box: 'show_box',
        show_name: 'show_name',
        show_similarity: 'show_similarity',
      };
      const dmFields: Record<string, keyof DualModelConfig> = {
        dm_enabled: 'enabled',
        dm_target_conf_threshold: 'target_conf_threshold',
        dm_crop_expand_pm: 'crop_expand_pm',
        dm_min: 'crop_min_size',
        dm_max_targets: 'max_targets',
        dm_interval_ms: 'interval_ms',
        dm_pad_value: 'pad_value',
      };

      const frPatch: Partial<FaceRecognitionConfig> = {};
      for (const [flat, real] of Object.entries(frFields)) {
        if (flat in patch) (frPatch as Record<string, unknown>)[real] = patch[flat];
      }
      const dmPatch: Partial<DualModelConfig> = {};
      for (const [flat, real] of Object.entries(dmFields)) {
        if (flat in patch) (dmPatch as Record<string, unknown>)[real] = patch[flat];
      }

      if (Object.keys(frPatch).length > 0) {
        await faceRecognition.setConfigReq(frPatch);
      }
      if (Object.keys(dmPatch).length > 0) {
        await faceRecognition.setDualModelConfigReq(dmPatch);
      }
      await loadConfig();
      if (successKey) toast.success(i18n._(successKey));
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_recognition.save_failed'));
    }
  };

  const numberRow = (
    labelKey: string,
    field: string,
    min: number,
    max: number,
    hintKey?: string,
  ) => (
    <div className="flex items-center justify-between gap-3 py-1.5">
      <div className="flex flex-col min-w-0">
        <Label className="text-sm shrink-0">{i18n._(labelKey)}</Label>
        {hintKey && <span className="text-xs text-gray-400">{i18n._(hintKey)}</span>}
      </div>
      <Input
        className="w-24 text-right shrink-0"
        type="number"
        min={min}
        max={max}
        value={paramDraft[field] !== undefined ? String(paramDraft[field]) : ''}
        onChange={(e) => {
          const parsed = Number((e.target as HTMLInputElement).value);
          if (!Number.isFinite(parsed)) return;
          setParamDraft((prev) => ({
            ...prev,
            [field]: Math.min(max, Math.max(min, parsed)),
          }));
        }}
      />
    </div>
  );

  const switchRow = (labelKey: string, field: string, instant = true) => (
    <div className="flex items-center justify-between gap-3 py-1.5">
      <Label className="text-sm">{i18n._(labelKey)}</Label>
      <Switch
        checked={Boolean(paramDraft[field])}
        onCheckedChange={(v) => {
          setParamDraft((prev) => ({ ...prev, [field]: v }));
          if (instant) patchConfig({ [field]: v });
        }}
      />
    </div>
  );

  const saveParams = async () => {
    setSavingParams(true);
    try {
      await patchConfig(paramDraft, 'sys.face_recognition.params_saved');
    } finally {
      setSavingParams(false);
    }
  };

  /* ---------- enrollment actions (manual, multi-pose) ---------- */
  const startEnroll = async () => {
    const name = enrollName.trim();
    if (!name) {
      toast.warning(i18n._('sys.face_recognition.enroll_name_required'));
      return;
    }
    if (stats?.model_b && !stats.model_b.loaded) {
      toast.warning(i18n._('sys.face_recognition.model_b_missing'));
      return;
    }
    try {
      await faceRecognition.enrollReq(name);
      lastEnrollStateRef.current = 'collecting';
      setEnrollState('collecting');
      setEnrollProgress({ collected: 0, needed: config?.enroll_samples ?? 0 });
      setCaptureArmed(false);
      toast.info(i18n._('sys.face_recognition.enroll_started'));
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_recognition.save_failed'));
    }
  };

  const captureSample = async () => {
    try {
      await faceRecognition.enrollCaptureReq();
      setCaptureArmed(true);
      toast.info(i18n._('sys.face_recognition.enroll_armed'));
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_recognition.save_failed'));
    }
  };

  const finishEnroll = async () => {
    try {
      await faceRecognition.enrollFinishReq();
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_recognition.save_failed'));
    }
  };

  const cancelEnroll = async () => {
    try {
      await faceRecognition.cancelEnrollReq();
      setEnrollState('idle');
      lastEnrollStateRef.current = 'idle';
      setCaptureArmed(false);
    } catch (error) {
      console.error(error);
    }
  };

  /* ---------- face db actions ---------- */
  const deleteFace = async (id: number) => {
    try {
      await faceRecognition.deleteFaceReq(id);
      await loadFaces();
      toast.success(i18n._('sys.face_recognition.face_deleted'));
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_recognition.save_failed'));
    }
  };

  const commitRename = async (id: number) => {
    const name = renameDraft.trim();
    if (!name) {
      setRenamingId(null);
      return;
    }
    try {
      await faceRecognition.renameFaceReq(id, name);
      await loadFaces();
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_recognition.save_failed'));
    } finally {
      setRenamingId(null);
    }
  };

  const clearFaces = async () => {
    setClearDialogOpen(false);
    try {
      await faceRecognition.clearFacesReq();
      await loadFaces();
      toast.success(i18n._('sys.face_recognition.faces_cleared'));
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_recognition.save_failed'));
    }
  };

  /* Model B upload/reload moved to the device-tool home page; this page
     only reads /face-recognition/status (stats.model_b) for the badge. */

  /* ---------- render helpers ---------- */
  const trackCard = (t: FrTrackInfo) => {
    const label = t.status === 'known'
      ? (t.name || `ID:${t.db_id}`)
      : t.status === 'unknown'
        ? i18n._('sys.face_recognition.unknown')
        : i18n._('sys.face_recognition.identifying');
    return (
      <div
        key={t.track_id}
        className="flex items-center justify-between gap-2 border border-gray-200 border-solid rounded-md px-3 py-2 bg-white"
      >
        <div className="flex items-center gap-2 min-w-0">
          <span className="font-mono font-bold text-gray-700">#{t.track_id}</span>
          <span className="text-xs text-gray-500">{pct(t.detection_confidence)}</span>
        </div>
        <div className="flex items-center gap-2 min-w-0">
          <span
            className={`px-2 py-0.5 rounded text-xs text-white truncate max-w-[140px] ${TRACK_STATUS_CLASS[t.status]}`}
            title={t.name}
          >
            {label}
          </span>
          {t.status === 'known' && (
            <span className="text-xs text-gray-600">{pct(t.similarity)}</span>
          )}
          {t.frontal === false && (
            <span
              className="px-1.5 py-0.5 rounded text-xs text-orange-600 bg-orange-50 shrink-0"
              title={t.pose_score !== undefined ? t.pose_score.toFixed(2) : undefined}
            >
              {i18n._('sys.face_recognition.not_frontal')}
            </span>
          )}
        </div>
      </div>
    );
  };

  const enrollPct = enrollProgress.needed > 0
    ? Math.round((enrollProgress.collected / enrollProgress.needed) * 100)
    : 0;

  return (
    <div className="flex flex-col gap-3 mt-4 bg-gray-100 p-4 rounded-lg">
      {/* video: recognition overlay (boxes + names) is burned into the stream */}
      <div
        className="relative bg-black w-full aspect-video mx-auto overflow-hidden"
        style={{ maxWidth: 'calc(62vh * 16 / 9)' }}
      >
        <video
          key={playerEpoch}
          ref={playerRef as React.Ref<HTMLVideoElement>}
          id="face-rec-player"
          className="w-full h-full"
          muted
          playsInline
          disableRemotePlayback
        />
        {playerLoading && (
          <div className="absolute left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2">
            <Loading placeholder="Loading..." />
          </div>
        )}
        {/* stats overlay (top-right) */}
        {stats && (
          <div className="absolute right-3 top-3 px-3 py-1.5 rounded bg-black/60 text-white text-xs font-medium backdrop-blur-sm">
            {i18n._('sys.face_recognition.st_now')} {stats.confirmed_tracks}
            {' · '}
            <span className="text-green-300">
              {i18n._('sys.face_recognition.st_known')} {stats.recognized_count}
            </span>
            {' · '}
            <span className="text-amber-300">
              {i18n._('sys.face_recognition.st_unknown')} {stats.unknown_count}
            </span>
            {' · '}
            {i18n._('sys.face_recognition.st_db')} {stats.db_count}
          </div>
        )}
        {/* enrollment progress (top-left) */}
        {enrollState === 'collecting' && (
          <div className="absolute left-3 top-3 px-3 py-2 rounded bg-black/70 text-white text-xs w-44 backdrop-blur-sm">
            <div className="mb-1">
              {i18n._('sys.face_recognition.enroll_collecting')}
              {' '}
              {enrollProgress.collected}/{enrollProgress.needed}
            </div>
            <Progress value={enrollPct} className="h-1.5" />
          </div>
        )}
        {/* Model B badge (bottom-right) */}
        {stats?.model_b && (
          <div
            className={`absolute right-3 bottom-3 px-2 py-1 rounded text-xs max-w-[60%] truncate ${
              stats.model_b.loaded
                ? 'bg-green-600/80 text-white'
                : 'bg-orange-600/80 text-white'
            }`}
            title={stats.model_b.name}
          >
            {stats.model_b.loaded
              ? `${i18n._('sys.face_recognition.model_b')} ${stats.model_b.name} (${stats.model_b.input_width}x${stats.model_b.input_height})`
              : i18n._('sys.face_recognition.model_b_missing')}
          </div>
        )}
      </div>

      <div className="w-full mx-auto flex flex-col gap-3" style={SECTION_MAX_W}>
        {/* live tracks */}
        <div className="border border-gray-200 border-solid rounded-md bg-white p-3 flex flex-col gap-3">
          <div className="flex items-center justify-between gap-2">
            <Label>{i18n._('sys.face_recognition.tracks_title')}</Label>
            <Button
              type="button"
              variant="outline"
              size="sm"
              onClick={async () => {
                try {
                  await faceRecognition.resetReq();
                } catch (error) {
                  console.error(error);
                }
              }}
            >
              {i18n._('sys.face_recognition.reset')}
            </Button>
          </div>
          {results && results.tracks.length > 0 ? (
            <div className="grid gap-2 md:grid-cols-2">
              {results.tracks.map(trackCard)}
            </div>
          ) : (
            <div className="text-xs text-gray-400 py-2 text-center">
              {i18n._('sys.face_recognition.no_tracks')}
            </div>
          )}
        </div>

        {/* enrollment (manual, multi-pose) */}
        <div className="border border-gray-200 border-solid rounded-md bg-white p-3 flex flex-col gap-3">
          <Label>{i18n._('sys.face_recognition.enroll_title')}</Label>
          <div className="flex items-center gap-2">
            <Input
              placeholder={i18n._('sys.face_recognition.enroll_name_placeholder')}
              value={enrollName}
              maxLength={31}
              disabled={enrollState === 'collecting' || enrollState === 'paused'}
              onChange={(e) => setEnrollName((e.target as HTMLInputElement).value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter' && enrollState !== 'collecting' && enrollState !== 'paused') startEnroll();
              }}
            />
            {(enrollState === 'collecting' || enrollState === 'paused') ? (
              <Button type="button" variant="outline" onClick={cancelEnroll}>
                {i18n._('sys.face_recognition.enroll_cancel')}
              </Button>
            ) : (
              <Button type="button" variant="primary" onClick={startEnroll}>
                {i18n._('sys.face_recognition.enroll_start')}
              </Button>
            )}
          </div>

          {enrollState === 'collecting' && (
            <div className="flex flex-col gap-2">
              <div className="flex items-center justify-between gap-2">
                <span className="text-sm">
                  {i18n._('sys.face_recognition.enroll_samples_label')}
                  {' '}
                  <span className="font-semibold">
                    {enrollProgress.collected}/{enrollProgress.needed}
                  </span>
                </span>
                {captureArmed && (
                  <span className="text-xs text-orange-500">
                    {i18n._('sys.face_recognition.enroll_wait_frontal')}
                  </span>
                )}
              </div>
              <Progress
                value={
                  enrollProgress.needed > 0
                    ? Math.round((enrollProgress.collected / enrollProgress.needed) * 100)
                    : 0
                }
                className="h-1.5"
              />
              <div className="flex items-center gap-2">
                <Button type="button" variant="primary" onClick={captureSample}>
                  {i18n._('sys.face_recognition.enroll_capture')}
                </Button>
                <Button
                  type="button"
                  variant="outline"
                  disabled={enrollProgress.collected < 2}
                  onClick={finishEnroll}
                >
                  {i18n._('sys.face_recognition.enroll_finish')}
                </Button>
              </div>
              <div className="text-xs text-gray-400">
                {i18n._('sys.face_recognition.enroll_hint_manual')}
              </div>
            </div>
          )}

          {enrollState === 'paused' && (
            /* Window expired with partial samples: nothing is lost - offer
             * resume / finish-with-what-we-have / discard. */
            <div className="flex flex-col gap-2">
              <div className="flex items-center justify-between gap-2">
                <span className="text-sm text-orange-500">
                  {i18n._('sys.face_recognition.enroll_paused_hint')}
                </span>
                <span className="text-sm font-semibold">
                  {enrollProgress.collected}/{enrollProgress.needed}
                </span>
              </div>
              <div className="flex items-center gap-2">
                <Button type="button" variant="primary" onClick={captureSample}>
                  {i18n._('sys.face_recognition.enroll_resume')}
                </Button>
                <Button
                  type="button"
                  variant="outline"
                  disabled={enrollProgress.collected < 2}
                  onClick={finishEnroll}
                >
                  {i18n._('sys.face_recognition.enroll_finish')}
                </Button>
              </div>
            </div>
          )}

          {enrollState !== 'collecting' && enrollState !== 'paused' && (
            <div className="text-xs text-gray-400">
              {i18n._('sys.face_recognition.enroll_hint')}
            </div>
          )}
        </div>

        {/* face database */}
        <div className="border border-gray-200 border-solid rounded-md bg-white p-3 flex flex-col gap-3">
          <div className="flex items-center justify-between gap-2">
            <Label>
              {i18n._('sys.face_recognition.db_title')}
              {faces ? ` (${faces.count})` : ''}
            </Label>
            <Button
              type="button"
              variant="outline"
              size="sm"
              disabled={!faces || faces.count === 0}
              onClick={() => setClearDialogOpen(true)}
            >
              {i18n._('sys.face_recognition.db_clear')}
            </Button>
          </div>
          {faces && faces.count > 0 ? (
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead className="w-16">ID</TableHead>
                  <TableHead>{i18n._('sys.face_recognition.db_name')}</TableHead>
                  <TableHead>{i18n._('sys.face_recognition.db_created')}</TableHead>
                  <TableHead className="w-28 text-right">
                    {i18n._('sys.face_recognition.db_actions')}
                  </TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {faces.faces.map((f) => (
                  <TableRow key={f.id}>
                    <TableCell className="font-mono text-xs">{f.id}</TableCell>
                    <TableCell>
                      {renamingId === f.id ? (
                        <Input
                          autoFocus
                          value={renameDraft}
                          maxLength={31}
                          onChange={(e) => setRenameDraft((e.target as HTMLInputElement).value)}
                          onBlur={() => commitRename(f.id)}
                          onKeyDown={(e) => {
                            if (e.key === 'Enter') commitRename(f.id);
                            if (e.key === 'Escape') setRenamingId(null);
                          }}
                        />
                      ) : (
                        <span className="truncate max-w-[160px] inline-block" title={f.name}>
                          {f.name}
                        </span>
                      )}
                    </TableCell>
                    <TableCell className="text-xs text-gray-500">
                      {f.created_at > 0
                        ? new Date(f.created_at * 1000).toLocaleString()
                        : '-'}
                    </TableCell>
                    <TableCell className="text-right">
                      <div className="inline-flex gap-1">
                        <Button
                          type="button"
                          variant="ghost"
                          size="sm"
                          onClick={() => {
                            setRenamingId(f.id);
                            setRenameDraft(f.name);
                          }}
                        >
                          {i18n._('sys.face_recognition.db_rename')}
                        </Button>
                        <Button
                          type="button"
                          variant="ghost"
                          size="sm"
                          className="text-red-600 hover:text-red-700"
                          onClick={() => deleteFace(f.id)}
                        >
                          {i18n._('sys.face_recognition.db_delete')}
                        </Button>
                      </div>
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          ) : (
            <div className="text-xs text-gray-400 py-2 text-center">
              {i18n._('sys.face_recognition.db_empty')}
            </div>
          )}
        </div>

        {/* Model B is managed (upload / reload) from the device-tool home
            page now - this page only surfaces its live status badge. */}

        {/* parameters */}
        <div className="border border-gray-200 border-solid rounded-md bg-white p-3 flex flex-col gap-3">
          <div className="flex items-center justify-between gap-2">
            <Label>{i18n._('sys.face_recognition.params_title')}</Label>
            <Button
              type="button"
              variant="primary"
              size="sm"
              disabled={savingParams}
              onClick={saveParams}
            >
              {i18n._('sys.face_recognition.params_save')}
            </Button>
          </div>

          <div>
            <p className="text-sm text-text-primary font-semibold mb-1">
              {i18n._('sys.face_recognition.params_matching')}
            </p>
            {numberRow('sys.face_recognition.match_threshold', 'match_threshold', 1, 99, 'sys.face_recognition.match_threshold_hint')}
            {numberRow('sys.face_recognition.confirm_votes', 'confirm_votes', 1, 10)}
            {numberRow('sys.face_recognition.enroll_samples', 'enroll_samples', 1, 10)}
            {numberRow('sys.face_recognition.enroll_min_similarity', 'enroll_min_similarity', 1, 99)}
            {numberRow('sys.face_recognition.pose_max_yaw', 'pose_max_yaw', 20, 99, 'sys.face_recognition.pose_max_yaw_hint')}
            {numberRow('sys.face_recognition.pose_max_roll', 'pose_max_roll', 10, 60)}
          </div>

          <Separator />

          <div>
            <p className="text-sm text-text-primary font-semibold mb-1">
              {i18n._('sys.face_recognition.params_dual_model')}
            </p>
            {switchRow('sys.face_recognition.dm_enabled', 'dm_enabled')}
            {numberRow('sys.face_recognition.dm_target_conf', 'dm_target_conf_threshold', 1, 99)}
            {numberRow('sys.face_recognition.dm_crop_expand', 'dm_crop_expand_pm', 0, 500, 'sys.face_recognition.dm_crop_expand_hint')}
            {numberRow('sys.face_recognition.dm_max_targets', 'dm_max_targets', 1, 4)}
            {numberRow('sys.face_recognition.dm_interval', 'dm_interval_ms', 0, 10000)}
          </div>

          <Separator />

          <div>
            <p className="text-sm text-text-primary font-semibold mb-1">
              {i18n._('sys.face_recognition.params_tracking')}
            </p>
            {numberRow('sys.face_recognition.track_iou', 'track_iou_threshold', 1, 99)}
            {numberRow('sys.face_recognition.track_max_missed', 'track_max_missed', 1, 50)}
          </div>

          <Separator />

          <div>
            <p className="text-sm text-text-primary font-semibold mb-1">
              {i18n._('sys.face_recognition.params_display')}
            </p>
            {switchRow('sys.face_recognition.show_box', 'show_box')}
            {switchRow('sys.face_recognition.show_name', 'show_name')}
            {switchRow('sys.face_recognition.show_similarity', 'show_similarity')}
          </div>
        </div>
      </div>

      {/* clear-all confirmation */}
      {clearDialogOpen && (
        <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/40">
          <div className="bg-white rounded-lg p-5 w-80 flex flex-col gap-4">
            <div className="font-semibold text-base">
              {i18n._('sys.face_recognition.db_clear_confirm_title')}
            </div>
            <div className="text-sm text-gray-500">
              {i18n._('sys.face_recognition.db_clear_confirm_body')}
            </div>
            <div className="flex justify-end gap-2">
              <Button type="button" variant="outline" onClick={() => setClearDialogOpen(false)}>
                {i18n._('sys.face_recognition.cancel')}
              </Button>
              <Button type="button" variant="primary" onClick={clearFaces}>
                {i18n._('sys.face_recognition.confirm')}
              </Button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
