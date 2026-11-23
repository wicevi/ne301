import { useEffect, useRef, useState } from 'react';
import { useLingui } from '@lingui/react';
import { useSearchParams } from 'react-router-dom';
import { Label } from '@/components/ui/label';
import { Separator } from '@/components/ui/separator';
import { Switch } from '@/components/ui/switch';
import { Button } from '@/components/ui/button';
import { NumberField } from '@/components/number-field';
import { Tooltip, TooltipContent, TooltipTrigger } from '@/components/tooltip';
import {
  Select, SelectContent, SelectItem, SelectTrigger, SelectValue,
} from '@/components/ui/select';
import SvgIcon from '@/components/svg-icon';
import Slider from '@/components/slider';
import { Input } from '@/components/ui/input';
import { toast } from 'sonner';
import captureSettings, {
  type CaptureUploadConfig, type CaptureMode, type CaptureStorage,
  type StoragePolicy, type UploadProto,
} from '@/services/api/captureSettings';
import hardwareManagement, { type SetHardwareInfoReq } from '@/services/api/hardware-management';
import deviceTool from '@/services/api/deviceTool';
import storageManagement from '@/services/api/storageManagement';
import systemSettings from '@/services/api/systemSettings';

const MINUTE_OPTIONS: { label: string; value: number }[] = [];
for (let h = 0; h < 24; h++) {
  for (let m = 0; m < 60; m += 30) {
    const v = h * 60 + m;
    const label = `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}`;
    MINUTE_OPTIONS.push({ label, value: v });
  }
}

/** "上传成功文件保留时间" dropdown options (hours). 0 = delete right after a
 * successful upload; 72000 = keep-forever sentinel (backend semantics). */
const KEEP_SENT_OPTIONS: { value: number; labelKey: string }[] = [
  { value: 0, labelKey: 'sys.capture_settings.keep_none' },
  { value: 12, labelKey: 'sys.capture_settings.keep_12h' },
  { value: 24, labelKey: 'sys.capture_settings.keep_1d' },
  { value: 168, labelKey: 'sys.capture_settings.keep_7d' },
  { value: 720, labelKey: 'sys.capture_settings.keep_1m' },
  { value: 2160, labelKey: 'sys.capture_settings.keep_3m' },
  { value: 4320, labelKey: 'sys.capture_settings.keep_6m' },
  { value: 8760, labelKey: 'sys.capture_settings.keep_1y' },
  { value: 26280, labelKey: 'sys.capture_settings.keep_3y' },
  { value: 72000, labelKey: 'sys.capture_settings.keep_forever' },
];

// Small helper rendering the label-side info tooltip (same pattern as JPEG质量).
function InfoTip({ text }: { text: string }) {
  return (
    <Tooltip mbEnhance>
      <TooltipTrigger>
        <div className="inline-flex h-4 w-4 shrink-0 items-center justify-center text-gray-500">
          <SvgIcon className="h-4 w-4 text-gray-500" icon="info" />
        </div>
      </TooltipTrigger>
      <TooltipContent className="max-w-80 text-pretty">
        <p>{text}</p>
      </TooltipContent>
    </Tooltip>
  );
}

export default function CaptureConfig() {
  const { i18n } = useLingui();
  const [searchParams] = useSearchParams();
  const [sdReady, setSdReady] = useState(false);
  /* Scroll target for the ?focus=sleep-wake deep link (deviceTool shortcut):
   * opens the collapsible and scrolls it into view once rendered. */
  const sleepSectionRef = useRef<HTMLDivElement>(null);

  const [cfg, setCfg] = useState<CaptureUploadConfig | null>(null);
  const [loading, setLoading] = useState(false);
  const [saving, setSaving] = useState(false);
  const [uploadSectionOpen, setUploadSectionOpen] = useState(true);
  const [cameraSectionOpen, setCameraSectionOpen] = useState(true);
  const [aiSectionOpen, setAiSectionOpen] = useState(true);
  /* Label-picker panel (80-class COCO lists would flood the page). */
  const [labelPickerOpen, setLabelPickerOpen] = useState(false);

  /* Power mode: the sleep-wake capture section only applies in low-power
   * mode; hide it entirely when the device runs full-speed. */
  const [powerMode, setPowerMode] = useState<string>('low_power');

  /* Deep link ?focus=sleep-wake (deviceTool shortcut): expand the sleep-wake
   * section and scroll it into view once it is rendered. The page layout
   * keeps shifting while configs load (skeletons replaced, sections grow),
   * so a single early scroll lands wrong — retry a few times. */
  const focusSleepWake = searchParams.get('focus') === 'sleep-wake';
  useEffect(() => {
    if (!focusSleepWake || powerMode === 'full_speed') return;
    setCameraSectionOpen(true);
    let cancelled = false;
    const tryScroll = (attempt: number) => {
      if (cancelled) return;
      sleepSectionRef.current?.scrollIntoView({ behavior: 'smooth', block: 'start' });
      if (attempt < 4) setTimeout(() => tryScroll(attempt + 1), 500);
    };
    const first = setTimeout(() => tryScroll(0), 300);
    return () => { cancelled = true; clearTimeout(first); };
  }, [focusSleepWake, powerMode]);

  /* ---- hardware capture params (local state, committed on global save) ---- */
  const [fastSkipFrames, setFastSkipFrames] = useState(0);
  const [fastResolution, setFastResolution] = useState(0);
  const [fastJpegQuality, setFastJpegQuality] = useState(85);
  const [captureStorageAi, setCaptureStorageAi] = useState(false);
  /* Snapshot of the camera params as loaded from the device, so saveAll can
   * tell whether the wake-time params actually changed. Only those need the
   * "reboot/next wake" warning; the upload config is hot-reloaded live. */
  const [initCam, setInitCam] = useState({ fastSkipFrames: 0, fastResolution: 0, fastJpegQuality: 85, captureStorageAi: false });

  /* Model class-name list (from /ai/status), for the AI-filter "allowed
   * result labels" chips. Empty when no model is loaded / the model has no
   * label table - then only the "All" chip is offered. */
  const [modelLabels, setModelLabels] = useState<string[]>([]);

  /* Available netifs (mirrors system settings "通讯方式"), fetched once for the
   * "上传网络" dropdown. Each item: { type: 'wifi'|'halow'|'cellular'|'poe', display_name } */
  const [commTypes, setCommTypes] = useState<{ type: string; display_name: string }[]>([]);

  const mode: CaptureMode = cfg?.mode ?? 'instant';
  const storage: CaptureStorage = cfg?.storage ?? 'auto';
  const policy: StoragePolicy = cfg?.policy ?? 'wrap';
  const proto: UploadProto = cfg?.upload_protocol ?? 'mqtt';
  const uploadNet: string = cfg?.upload_network ?? 'default';

  const load = async () => {
    setLoading(true);
    try {
      const [res, hwRes, stRes, netRes, aiRes] = await Promise.all([
        captureSettings.getUploadConfig(),
        hardwareManagement.getHardwareInfoReq(),
        storageManagement.getStorage(),
        systemSettings.getNetworkStatusReq(),
        deviceTool.getAiStatusReq().catch(() => null),
      ]);
      const lbl = (aiRes as any)?.data?.model?.labels;
      setModelLabels(Array.isArray(lbl) ? lbl.filter((l: unknown) => typeof l === 'string') : []);

      setCfg(res.data);
      const arr = netRes?.data?.available_comm_types;
      setCommTypes(Array.isArray(arr) ? arr.map((t: any) => ({ type: t.type, display_name: t.display_name })) : []);
      const hw = hwRes.data;
      const camInit = {
        fastSkipFrames: typeof hw.fast_capture_skip_frames === 'number' ? hw.fast_capture_skip_frames : 0,
        fastResolution: typeof hw.fast_capture_resolution === 'number' ? hw.fast_capture_resolution : 0,
        fastJpegQuality: typeof hw.fast_capture_jpeg_quality === 'number' ? hw.fast_capture_jpeg_quality : 85,
        captureStorageAi: typeof hw.capture_storage_ai === 'boolean' ? hw.capture_storage_ai : false,
      };
      setFastSkipFrames(camInit.fastSkipFrames);
      setFastResolution(camInit.fastResolution);
      setFastJpegQuality(camInit.fastJpegQuality);
      setCaptureStorageAi(camInit.captureStorageAi);
      setInitCam(camInit);
      const st: any = stRes.data;
      setSdReady(st?.sd_card_connected === true);

      try {
        const pm = await deviceTool.getPowerModeReq();
        if (pm?.data?.current_mode) setPowerMode(pm.data.current_mode);
      } catch (e) {
        console.error('load power mode', e);
      }
    } catch (e) {
      console.error(e);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => { load(); }, []);

  const patch = (k: keyof CaptureUploadConfig, v: unknown) => {
    setCfg((prev) => (prev ? { ...prev, [k]: v } : prev));
  };

  /* Parsed ai_filter_labels ('' = all allowed). */
  const allowedLabels = (cfg?.ai_filter_labels ?? '')
    .split(',')
    .map((x) => x.trim())
    .filter(Boolean);

  /* Global save: commits BOTH upload config and camera params. The upload
   * config and capture_storage_ai are hot-reloaded (the runtime capture
   * path re-reads them on every capture); only the wake-time camera params
   * (fast_capture_*) need a reboot/next-wake, so we warn only when those
   * actually changed. */
  const saveAll = async () => {
    if (!cfg) return;
    const wakeParamsChanged = fastSkipFrames !== initCam.fastSkipFrames
      || fastResolution !== initCam.fastResolution
      || fastJpegQuality !== initCam.fastJpegQuality;
    setSaving(true);
    try {
      await captureSettings.setUploadConfig(cfg);
      await hardwareManagement.setHardwareInfoReq({
        fast_capture_skip_frames: fastSkipFrames,
        fast_capture_resolution: fastResolution,
        fast_capture_jpeg_quality: fastJpegQuality,
        capture_storage_ai: captureStorageAi,
      } as SetHardwareInfoReq);
      if (wakeParamsChanged) {
        toast.warning(i18n._('sys.capture_settings.saved_reboot_hint'));
      } else {
        toast.success(i18n._('sys.capture_settings.saved_immediate_hint'));
      }
      setInitCam({ fastSkipFrames, fastResolution, fastJpegQuality, captureStorageAi });
    } catch (e) {
      console.error(e);
      toast.error(i18n._('sys.capture_settings.save_failed') ?? 'Save failed');
    } finally {
      setSaving(false);
    }
  };

  if (loading || !cfg) {
    return (
<div className="flex flex-col gap-2 mt-4 bg-gray-100 p-4 rounded-lg">
      <div className="h-8 bg-gray-200 rounded animate-pulse" />
      <div className="h-8 bg-gray-200 rounded animate-pulse" />
</div>
);
  }

  const isLocalOnly = mode === 'local_only';
  const isScheduled = mode === 'scheduled';
  const isBatch = mode === 'batch';
  const isInstant = mode === 'instant';
  const showUploadSection = !isLocalOnly;
  const showRetry = showUploadSection && storage !== 'none';
  const showStorageSection = storage !== 'none';
  const showScheduleNodes = isScheduled;

  return (
    <div className="flex flex-col gap-2 mt-4 bg-gray-100 p-4 rounded-lg">
      {/* ============ Upload params (collapsible) ============ */}
      <div className="flex flex-col gap-2">
        <button
          type="button"
          className="flex w-full items-center justify-between text-left"
          onClick={() => setUploadSectionOpen(o => !o)}
        >
          <Label>{i18n._('sys.capture_settings.section_upload_mode')}</Label>
          <span className="inline-flex h-4 w-4 shrink-0 items-center justify-center text-gray-500">
            <SvgIcon
              icon="right"
              className={`h-4 w-4 transition-transform duration-200 ${
                uploadSectionOpen ? 'rotate-90' : 'rotate-0'
              }`}
            />
          </span>
        </button>
        {uploadSectionOpen && (
          <div className="border border-gray-200 border-solid p-4 rounded-md mt-2 flex flex-col gap-2">
            {/* mode — storage=none only allows instant upload, other modes disabled */}
            <div className="flex justify-between gap-4 items-center">
              <Label>{i18n._('sys.capture_settings.mode')}</Label>
              <Select
                value={mode}
                onValueChange={(v) => {
                patch('mode', v as CaptureMode);
                if (v === 'local_only') { patch('retry_enable', false); }
                // Defensive: leaving instant while storage is "none" would be
                // rejected by the backend - fall back to auto storage.
                if (v !== 'instant' && cfg?.storage === 'none') {
                  patch('storage', 'auto');
                }
              }}
              >
                <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 w-fit">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="instant">{i18n._('sys.capture_settings.mode_instant')}</SelectItem>
                  <SelectItem value="batch" disabled={storage === 'none'}>
                    {i18n._('sys.capture_settings.mode_batch')}
                  </SelectItem>
                  <SelectItem value="scheduled" disabled={storage === 'none'}>
                    {i18n._('sys.capture_settings.mode_scheduled')}
                  </SelectItem>
                  <SelectItem value="local_only" disabled={storage === 'none'}>
                    {i18n._('sys.capture_settings.mode_local_only')}
                  </SelectItem>
                </SelectContent>
              </Select>
            </div>
            <Separator />

            {/* storage */}
            <div className="flex justify-between gap-4 items-center">
              <div className="flex items-center gap-2">
                <Label>{i18n._('sys.capture_settings.storage')}</Label>
                <InfoTip text={i18n._('sys.capture_settings.storage_tip').toString()} />
              </div>
              <Select
                value={storage}
                onValueChange={(v) => patch('storage', v as CaptureStorage)}
              >
                <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 w-fit">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="auto">{i18n._('sys.capture_settings.storage_auto')}</SelectItem>
                  <SelectItem value="flash">{i18n._('sys.capture_settings.storage_flash')}</SelectItem>
                  <SelectItem value="sd" disabled={!sdReady}>
                    {i18n._('sys.capture_settings.storage_sd')}
                  </SelectItem>
                  <SelectItem value="none" disabled={!isInstant}>
                    {i18n._('sys.capture_settings.storage_none')}
                  </SelectItem>
                </SelectContent>
              </Select>
            </div>

            {/* flash record cap — editable, only shown when records can land on flash */}
            {(storage === 'flash' || storage === 'auto') && (
              <div className="flex justify-between gap-4 items-center">
                <div className="flex items-center gap-2">
                  <Label>{i18n._('sys.capture_settings.flash_max_records')}</Label>
                  <InfoTip text={i18n._('sys.capture_settings.flash_max_records_hint').toString()} />
                </div>
                <NumberField
                  className="w-24 text-right"
                  min={16}
                  max={256}
                  value={cfg.flash_max_records ?? 32}
                  onCommit={(v) => patch('flash_max_records', v)}
                />
              </div>
            )}
            <Separator />

            {/* policy + capture_storage_ai (only when storage != none) */}
            {showStorageSection && (
              <div className="flex justify-between gap-4 items-center">
                  <div className="flex items-center gap-2">
                    <Label>{i18n._('sys.capture_settings.policy')}</Label>
                    <InfoTip text={i18n._('sys.capture_settings.policy_tip').toString()} />
                  </div>
                  <Select
                    value={policy}
                    onValueChange={(v) => patch('policy', v as StoragePolicy)}
                  >
                    <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 w-fit">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="wrap">{i18n._('sys.capture_settings.policy_wrap')}</SelectItem>
                      <SelectItem value="stop">{i18n._('sys.capture_settings.policy_stop')}</SelectItem>
                    </SelectContent>
                  </Select>
              </div>
            )}

            {/* upload protocol + batch + schedule */}
            {showUploadSection && (
              <>
                <div className="flex justify-between gap-4 items-center">
                  <Label>{i18n._('sys.capture_settings.upload_protocol')}</Label>
                  <Select
                    value={proto}
                    onValueChange={(v) => patch('upload_protocol', v as UploadProto)}
                  >
                    <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 w-fit">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="mqtt">{i18n._('sys.capture_settings.proto_mqtt')}</SelectItem>
                      <SelectItem value="webhook">{i18n._('sys.capture_settings.proto_webhook')}</SelectItem>
                    </SelectContent>
                  </Select>
                </div>
                <Separator />

                <div className="flex justify-between gap-4 items-center">
                  <div className="flex items-center gap-2">
                    <Label>{i18n._('sys.capture_settings.upload_network')}</Label>
                    <InfoTip text={i18n._('sys.capture_settings.upload_net_hint').toString()} />
                  </div>
                  <Select
                    value={uploadNet}
                    onValueChange={(v) => patch('upload_network', v)}
                  >
                    <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 w-fit">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="default">{i18n._('sys.capture_settings.upload_net_default')}</SelectItem>
                      {commTypes.map((t) => (
                        <SelectItem key={t.type} value={t.type}>{t.display_name}</SelectItem>
                      ))}
                    </SelectContent>
                  </Select>
                </div>
                <Separator />

                {isBatch && (
                  <>
                    <div className="flex justify-between gap-4 items-center">
                      <Label>{i18n._('sys.capture_settings.batch_count')}</Label>
                      <NumberField
                        className="w-24 text-right"
                        min={2}
                        max={20}
                        value={cfg.batch_count}
                        onCommit={(v) => patch('batch_count', v)}
                      />
                    </div>
                    <Separator />
                  </>
                )}

                {showScheduleNodes && (
                  <>
                    <div className="flex flex-col gap-2">
                      <Label>{i18n._('sys.capture_settings.schedule_minutes')}</Label>
                      <div className="flex flex-wrap gap-2">
                        {(cfg.schedule_minutes ?? []).slice(0, 8).map((min, idx) => (
                          <div key={idx} className="flex items-center gap-1">
                            <Select
                              value={String(min)}
                              onValueChange={(v) => {
                              const arr = [...(cfg.schedule_minutes ?? [])];
                              arr[idx] = Number(v);
                              patch('schedule_minutes', arr);
                            }}
                            >
                              <SelectTrigger className="border px-2 py-1 text-sm w-24">
                                <SelectValue />
                              </SelectTrigger>
                              <SelectContent>
                                {MINUTE_OPTIONS.map((o) => (
                                  <SelectItem key={o.value} value={String(o.value)}>{o.label}</SelectItem>
                                ))}
                              </SelectContent>
                            </Select>
                            <button
                              type="button"
                              className="text-red-500 text-xs"
                              onClick={() => {
                                const arr = (cfg.schedule_minutes ?? []).filter((_, i) => i !== idx);
                                patch('schedule_minutes', arr);
                              }}
                            >✕
                            </button>
                          </div>
                        ))}
                      </div>
                      {(cfg.schedule_minutes?.length ?? 0) < 8 && (
                        <Button
                          variant="outline"
                          size="sm"
                          className="w-fit"
                          onClick={() => {
                            const arr = [...(cfg.schedule_minutes ?? []), 480];
                            patch('schedule_minutes', arr);
                          }}
                        >
                          + {i18n._('sys.capture_settings.add_node')}
                        </Button>
                      )}
                      <p className="text-xs text-gray-500 mt-1">
                        {i18n._('sys.capture_settings.schedule_hint')}
                      </p>
                    </div>
                    <Separator />
                  </>
                )}
              </>
            )}

            {/* retry */}
            {showRetry && (
              <>
                <div className="flex justify-between gap-4 items-center">
                  <div className="flex items-center gap-2">
                    <Label>{i18n._('sys.capture_settings.retry_enable')}</Label>
                  </div>
                  <Switch
                    checked={cfg.retry_enable}
                    onCheckedChange={(v) => patch('retry_enable', v)}
                  />
                </div>
                <Separator />

                {cfg.retry_enable && (
                  <>
                    <div className="flex justify-between gap-4 items-center">
                      <Label>{i18n._('sys.capture_settings.retry_max_attempts')}</Label>
                      <NumberField
                        className="w-24 text-right"
                        min={0}
                        max={20}
                        value={cfg.retry_max_attempts}
                        onCommit={(v) => patch('retry_max_attempts', v)}
                      />
                    </div>
                    <Separator />
                  </>
                )}
              </>
            )}

            {/* keep_sent_hours — retention of successfully uploaded files.
                Only meaningful when files are both uploaded and stored. */}
            {showUploadSection && showStorageSection && (
              <div className="flex justify-between gap-4 items-center">
                <div className="flex items-center gap-2">
                  <Label>{i18n._('sys.capture_settings.keep_sent_label')}</Label>
                  <InfoTip text={i18n._('sys.capture_settings.keep_sent_tip').toString()} />
                </div>
                <Select
                  value={String(cfg.keep_sent_hours)}
                  onValueChange={(v) => patch('keep_sent_hours', Number(v))}
                >
                  <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 w-fit">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {KEEP_SENT_OPTIONS.map((o) => (
                      <SelectItem key={o.value} value={String(o.value)}>
                        {i18n._(o.labelKey)}
                      </SelectItem>
                    ))}
                    {/* Value set by an older UI / API (raw hours): keep it selectable */}
                    {!KEEP_SENT_OPTIONS.some((o) => o.value === cfg.keep_sent_hours) && (
                      <SelectItem value={String(cfg.keep_sent_hours)}>
                        {i18n._('sys.capture_settings.keep_custom').replace('{hours}', String(cfg.keep_sent_hours))}
                      </SelectItem>
                    )}
                  </SelectContent>
                </Select>
              </div>
            )}
          </div>
        )}
      </div>

      {/* ============ AI configuration (collapsible, same level as upload params) ============ */}
      {/* AI participation in captures: master switch + annotated result
       * image + result filter. The filter gates every capture (storage AND
       * upload) regardless of the storage mode above, so this section is
       * always visible. */}
      <Separator />
      <div className="flex flex-col gap-2">
        <button
          type="button"
          className="flex w-full items-center justify-between text-left"
          onClick={() => setAiSectionOpen(o => !o)}
        >
          <Label>{i18n._('sys.capture_settings.ai_config_section')}</Label>
          <span className="inline-flex h-4 w-4 shrink-0 items-center justify-center text-gray-500">
            <SvgIcon
              icon="right"
              className={`h-4 w-4 transition-transform duration-200 ${
                aiSectionOpen ? 'rotate-90' : 'rotate-0'
              }`}
            />
          </span>
        </button>
        {aiSectionOpen && (
          <div className="border border-gray-200 border-solid p-4 rounded-md mt-2 flex flex-col gap-2">
            {/* annotated result image — hidden when storage=none (nothing is saved) */}
            {showStorageSection && (
              <div className="flex flex-col gap-2">
                  <div className="flex justify-between gap-4 items-center">
                    <div className="flex items-center gap-2">
                      <Label>{i18n._('sys.capture_settings.capture_storage_ai_drawn')}</Label>
                      <InfoTip text={i18n._('sys.capture_settings.capture_storage_ai_drawn_tip').toString()} />
                    </div>
                    <Switch
                      checked={captureStorageAi}
                      onCheckedChange={setCaptureStorageAi}
                    />
                  </div>
                  <Separator />
                  <div className="flex justify-between gap-4 items-center">
                    <div className="flex items-center gap-2">
                      <Label>{i18n._('sys.capture_settings.ai_filter_switch')}</Label>
                      <InfoTip text={i18n._('sys.capture_settings.ai_filter_enable_tip').toString()} />
                    </div>
                    <Switch
                      checked={cfg.ai_filter_enable ?? false}
                      onCheckedChange={(v) => patch('ai_filter_enable', v)}
                    />
                  </div>
                  {cfg.ai_filter_enable && (
                    <>
                      <Separator />
                      <div className="w-full flex items-center justify-between gap-4">
                        <div className="flex items-center gap-2">
                          <Label>{i18n._('sys.capture_settings.ai_filter_threshold')}</Label>
                          <InfoTip text={i18n._('sys.capture_settings.ai_filter_threshold_tip').toString()} />
                        </div>
                        <div className="flex items-center gap-2 flex-1">
                          <Slider
                            className="flex-1 min-w-16"
                            value={cfg.ai_filter_threshold ?? 50}
                            max={100}
                            step={1}
                            min={1}
                            onChange={(value) => patch('ai_filter_threshold', value)}
                          />
                          <Input
                            className="w-16"
                            type="number"
                            min={1}
                            max={100}
                            step={1}
                            value={cfg.ai_filter_threshold ?? 50}
                            onChange={(e) => {
                              const v = parseInt((e.target as HTMLInputElement).value || '1', 10);
                              patch('ai_filter_threshold', Math.min(100, Math.max(1, v)));
                            }}
                          />
                        </div>
                      </div>
                      <Separator />
                      {/* Allowed result labels: the SELECTION lives in its own
                       * block (chip click removes); the candidate list (80
                       * classes on COCO models) collapses behind a toggle and
                       * scrolls when open, so it never floods the page. */}
                      <div className="flex flex-col gap-2">
                        <div className="flex items-center gap-2">
                          <Label>{i18n._('sys.capture_settings.ai_filter_labels')}</Label>
                          <InfoTip text={i18n._('sys.capture_settings.ai_filter_labels_tip').toString()} />
                        </div>
                        {/* selected results block */}
                        <div className="border border-gray-200 border-solid rounded-md p-2 flex flex-wrap gap-2 items-center min-h-9">
                          {allowedLabels.length === 0 ? (
                            <span className="text-sm px-2 py-1 rounded-md bg-primary text-white">
                              {i18n._('sys.capture_settings.ai_filter_labels_all')}
                            </span>
                          ) : allowedLabels.map((l) => (
                            <Button
                              key={l}
                              size="sm"
                              variant="primary"
                              onClick={() => {
                                /* removing from the selection block */
                                patch(
'ai_filter_labels',
                                  allowedLabels.filter((x) => x !== l).join(',')
);
                              }}
                            >
                              {l} ✕
                            </Button>
                          ))}
                        </div>
                        {/* collapsible candidate picker */}
                        <button
                          type="button"
                          className="flex items-center gap-1 text-sm text-gray-600 w-fit"
                          onClick={() => setLabelPickerOpen(o => !o)}
                        >
                          <SvgIcon
                            icon="right"
                            className={`h-3.5 w-3.5 transition-transform duration-200 ${
                              labelPickerOpen ? 'rotate-90' : 'rotate-0'
                            }`}
                          />
                          {i18n._('sys.capture_settings.ai_filter_labels_pick')}
                        </button>
                        {labelPickerOpen && (
                          <div className="border border-gray-200 border-solid rounded-md p-2 flex flex-wrap gap-2 max-h-40 overflow-y-auto">
                            <Button
                              size="sm"
                              variant={allowedLabels.length === 0 ? 'primary' : 'outline'}
                              onClick={() => patch('ai_filter_labels', '')}
                            >
                              {i18n._('sys.capture_settings.ai_filter_labels_all')}
                            </Button>
                            {modelLabels.map((l) => (
                              <Button
                                key={l}
                                size="sm"
                                variant={allowedLabels.includes(l) ? 'primary' : 'outline'}
                                onClick={() => {
                                  const next = allowedLabels.includes(l)
                                    ? allowedLabels.filter((x) => x !== l)
                                    : [...allowedLabels, l];
                                  patch('ai_filter_labels', next.join(','));
                                }}
                              >
                                {l}
                              </Button>
                            ))}
                          </div>
                        )}
                      </div>
                    </>
                  )}
              </div>
            )}
          </div>
        )}
      </div>

      {/* ============ Camera params (collapsible) ============ */}
      {/* Hidden in full-speed power mode: the sleep-wake quick path this
       * section configures never runs while the device stays awake. */}
      {powerMode !== 'full_speed' && (
      <>
      <Separator />
      <div className="flex flex-col gap-2" ref={sleepSectionRef}>
        <button
          type="button"
          className="flex w-full items-center justify-between text-left"
          onClick={() => setCameraSectionOpen(o => !o)}
        >
          <div className="flex items-center gap-2">
            <Label>{i18n._('sys.capture_settings.section_capture_params')}</Label>
            <Tooltip mbEnhance>
              <TooltipTrigger>
                <div className="inline-flex h-4 w-4 shrink-0 items-center justify-center text-gray-500">
                  <SvgIcon className="h-4 w-4 text-gray-500" icon="info" />
                </div>
              </TooltipTrigger>
              <TooltipContent className="max-w-80 text-pretty">
                <p>{i18n._('sys.hardware_management.capture_config_tip')}</p>
              </TooltipContent>
            </Tooltip>
          </div>
          <span className="inline-flex h-4 w-4 shrink-0 items-center justify-center text-gray-500">
            <SvgIcon
              icon="right"
              className={`h-4 w-4 transition-transform duration-200 ${
                cameraSectionOpen ? 'rotate-90' : 'rotate-0'
              }`}
            />
          </span>
        </button>
        {cameraSectionOpen && (
          <div className="border border-gray-200 border-solid p-4 rounded-md mt-2 flex flex-col gap-2">
            {/* skip frames — local state, committed on global save */}
            <div className="flex justify-between gap-4 items-center">
              <Label>{i18n._('sys.hardware_management.fast_capture_skip_frames')}</Label>
              <NumberField
                className="w-24 text-right"
                min={0}
                max={300}
                value={fastSkipFrames}
                onCommit={setFastSkipFrames}
              />
            </div>
            <Separator />

            {/* resolution */}
            <div className="flex justify-between gap-4 items-center">
              <Label>{i18n._('sys.hardware_management.fast_capture_resolution')}</Label>
              <Select
                value={String(fastResolution)}
                onValueChange={(v) => setFastResolution(Number(v ?? '0'))}
              >
                <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 w-32">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="0">1280x720</SelectItem>
                  <SelectItem value="1">1920x1080</SelectItem>
                  <SelectItem value="2">2688x1520</SelectItem>
                </SelectContent>
              </Select>
            </div>
            <Separator />

            {/* jpeg quality (with tooltip) */}
            <div className="flex justify-between gap-4 items-center">
              <div className="flex min-w-0 flex-1 items-center gap-2">
                <Label className="shrink-0">
                  {i18n._('sys.hardware_management.fast_capture_jpeg_quality')}
                </Label>
                <Tooltip mbEnhance>
                  <TooltipTrigger>
                    <div className="inline-flex h-4 w-4 shrink-0 items-center justify-center text-gray-500">
                      <SvgIcon className="h-4 w-4 text-gray-500" icon="info" />
                    </div>
                  </TooltipTrigger>
                  <TooltipContent className="max-w-80 text-pretty">
                    <p>{i18n._('sys.hardware_management.capture_jpeg_quality_tip')}</p>
                  </TooltipContent>
                </Tooltip>
              </div>
              <NumberField
                className="w-24 text-right"
                min={1}
                max={100}
                value={fastJpegQuality}
                onCommit={setFastJpegQuality}
              />
            </div>
          </div>
        )}
      </div>
      </>
      )}

      {/* ============ Global save (commits both upload config + camera params) ============ */}
      <Button variant="primary" disabled={saving} onClick={saveAll} className="mt-4">
        {saving ? '...' : i18n._('common.save')}
      </Button>
    </div>
  );
}
