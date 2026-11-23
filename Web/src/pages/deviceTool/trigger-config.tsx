import { useEffect, useState } from 'preact/hooks';
import { Separator } from '@/components/ui/separator';
import { Card, CardContent } from '@/components/ui/card';
import { Switch } from '@/components/ui/switch';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Skeleton } from '@/components/ui/skeleton';
import { Tooltip, TooltipContent, TooltipTrigger } from '@/components/tooltip';
import { Button } from '@/components/ui/button';
import TimePicker from '@/components/time-picker';
import { NumberField } from '@/components/number-field';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { useLingui } from '@lingui/react';
import SvgIcon from '@/components/svg-icon';
import Slider from '@/components/slider';
import deviceTool, { type PirConfigReq } from '@/services/api/deviceTool';
import { toast } from 'sonner';

type TriggerConfigType = {
  pir_trigger: {
    enable: boolean;
    device_type?: 'pir' | 'io';
    trigger_type:
    | 'rising_edge'
    | 'falling_edge'
    | 'high_level'
    | 'low_level'
    | 'both_edges';
    sensitivity_level: number;
    ignore_time_s: number;
    pulse_count: number;
    window_time_s: number;
    disable_in_preview: boolean;
  };
  timer_trigger: {
    enable: boolean;
    capture_mode: string;
    interval_sec: number;
    time_node_count: number;
    time_node: string[];
    weekdays: number[];
    interval_mode: 'normal' | 'scheduled';
    start_time: string;
    end_time?: string; // Scheduled mode daily window end "HH:MM"; 00:00 = ends at midnight (full day is start T with end T-1min); absent = old firmware, falls back to a full-day display
    anchor?: string; // Normal interval mode daily grid anchor "HH:MM" (device stamps current time-of-day when unset)
    next_capture_at?: number; // Read-only, computed by the device
  };
  remote_trigger: {
    enable: boolean;
  };
};

export type { TriggerConfigType as TriggerConfig };

type TriggerConfigProps = {
  childeRef: React.RefObject<HTMLDivElement>;
};

// "HH:MM" string for time-of-day pickers
const toHHMM = (d: Date) => {
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${pad(d.getHours())}:${pad(d.getMinutes())}`;
};

// seconds-of-day from an "HH:MM" string
const hhmmToSec = (s: string) => {
  const [h, m] = s.split(':').map(Number);
  return (h || 0) * 3600 + (m || 0) * 60;
};

/* ==================== PIR helpers ==================== */

// Sensitivity register range on the PIR conditioning IC (smaller = more
// sensitive). Values above ~120 are effectively saturated (insensitive), so
// the UI maps 1..100 onto 120..10. Register values outside the range (old NVS
// data) clamp to the nearest end.
const SENS_REG_MIN = 10;
const SENS_REG_MAX = 120;
const sensClamp = (reg: number) => Math.min(SENS_REG_MAX, Math.max(SENS_REG_MIN, reg));
const sensRegToUi = (reg: number) => {
  const r = sensClamp(reg);
  return Math.round(1 + ((SENS_REG_MAX - r) * 99) / (SENS_REG_MAX - SENS_REG_MIN));
};
const sensUiToReg = (ui: number) => Math.round(SENS_REG_MAX - ((ui - 1) * (SENS_REG_MAX - SENS_REG_MIN)) / 99);

// Window register 0-3 -> actual seconds 2/4/6/8 (2 + 2*reg)
const PIR_WINDOW_SECONDS = [2, 4, 6, 8] as const;
// Ignore-time register 0-15 -> actual seconds 0.5..8.0 (0.5 + 0.5*reg)
const pirIgnoreSeconds = (reg: number) => 0.5 + 0.5 * reg;

// Quick presets - register values from the sensor vendor's recommended
// engineering table (theoretical starting points, to be tuned by field tests).
// sens = SENS register, pulse = actual pulse count (1-4), window = seconds.
/* Four scenarios matching the approved product prototype. SENS register
 * values converted with the PROTOTYPE's mapping (>=50%: 60-(p-50), <50%:
 * 255-(p/50)*195): 80%→30, 50%→60, 88%→22, 70%→40. pulse = actual pulse
 * count (1-4), window = seconds, ignore = cooldown seconds. */
const PIR_PRESETS = [
  { key: 'indoor', sens: 30, pulse: 2, window: 2, ignore: 4 },
  { key: 'outdoor', sens: 60, pulse: 3, window: 2, ignore: 6 },
  { key: 'fast', sens: 22, pulse: 1, window: 2, ignore: 3 },
  { key: 'power', sens: 40, pulse: 2, window: 2, ignore: 8 },
] as const;

/* Sensitivity tier + estimated detection distance for the hint line under
 * the slider (field estimate, mirroring the vendor's reference table). */
const sensTierKey = (uiPct: number) => {
  if (uiPct >= 85) return 'sys.device_tool.sens_tier_ultra';
  if (uiPct >= 65) return 'sys.device_tool.sens_tier_high';
  if (uiPct >= 40) return 'sys.device_tool.sens_tier_mid';
  return 'sys.device_tool.sens_tier_low';
};
const sensEstDistance = (uiPct: number) => 1.5 + (uiPct / 100) * 8.5;

function PIRSkeleton() {
  return (
    <div className="flex flex-col gap-2 justify-between items-center">
      <Skeleton className="w-full h-10 rounded-md"></Skeleton>
      <Skeleton className="w-full h-10 rounded-md"></Skeleton>
      <Skeleton className="w-full h-10 rounded-md"></Skeleton>
      <Skeleton className="w-full h-10 rounded-md"></Skeleton>
    </div>
  );
}

export default function TriggerConfig({ childeRef }: TriggerConfigProps) {
  const { i18n } = useLingui();
  const { configTriggerConfigReq, configTimerTriggerReq, configRemoteTriggerReq, getTriggerConfigReq } = deviceTool;
  const [intervalCaptureTime, setIntervalCaptureTime] = useState(10);
  const [intervalCaptureTimeUnit, setIntervalCaptureTimeUnit] = useState('hour');
  // Raw text while editing the interval field; null = not editing (show the
  // committed value). Parse + clamp happen once on blur — clamping inside
  // onChange rewrites the field mid-typing (clear → forced "1", then typing
  // "30" shows "130"), making values unreachable.
  const [intervalDraft, setIntervalDraft] = useState<string | null>(null);
  const [scheduledStartTime, setScheduledStartTime] = useState('08:00');
  const [scheduledEndTime, setScheduledEndTime] = useState('23:59');
  // Daily grid anchor "HH:MM" for normal interval mode — a time-of-day, no
  // date: the lattice restarts at this time every day. Defaults to the
  // current time when the device hasn't stamped one yet.
  const [anchorInput, setAnchorInput] = useState(() => toHHMM(new Date()));
  const [savePirTriggerLoading, setSavePirTriggerLoading] = useState(false);
  const [PIRLoading, setPIRLoading] = useState(false);
  // Advanced PIR options (SENS register / disable-in-preview) hidden for now —
  // re-enable when field tuning needs them again.
  // const [pirAdvancedOpen, setPirAdvancedOpen] = useState(false);
  const [savedNextCaptureAt, setSavedNextCaptureAt] = useState<number | null>(
    null
  );
  // 1s tick so the next-capture preview tracks the current time
  const [, setNowTick] = useState(0);
  useEffect(() => {
    const timer = setInterval(() => setNowTick(v => v + 1), 1000);
    return () => clearInterval(timer);
  }, []);
  const WeekUnitMap = new Map([
    [0, i18n._('common.everyday').toString()],
    [1, i18n._('common.monday').toString()],
    [2, i18n._('common.tuesday').toString()],
    [3, i18n._('common.wednesday').toString()],
    [4, i18n._('common.thursday').toString()],
    [5, i18n._('common.friday').toString()],
    [6, i18n._('common.saturday').toString()],
    [7, i18n._('common.sunday').toString()],
  ]);
  const [triggerConfig, setTriggerConfig] = useState<TriggerConfigType>({
    pir_trigger: {
      enable: true,
      trigger_type: 'rising_edge',
      sensitivity_level: 30,
      ignore_time_s: 7,
      pulse_count: 1,
      window_time_s: 0,
      disable_in_preview: false,
    },
    timer_trigger: {
      enable: false,
      capture_mode: 'interval',
      interval_sec: 60,
      time_node_count: 0,
      time_node: [],
      weekdays: [],
      interval_mode: 'normal',
      start_time: '08:00',
    },
    remote_trigger: {
      enable: false,
    },
  });
  const getPirConfig = async () => {
    try {
      setPIRLoading(true);
      const res = await getTriggerConfigReq();
      setTriggerConfig(res.data);
      setSavedNextCaptureAt(res.data.timer_trigger?.next_capture_at ?? null);
    } catch (error) {
      console.error('getPirConfig', error);
      throw error;
    } finally {
      setPIRLoading(false);
    }
  };
  useEffect(() => {
    getPirConfig();
  }, []);
  const initIntervalCaptureTime = () => {
    if (!triggerConfig.timer_trigger) return;

    if (
      (triggerConfig.timer_trigger.interval_sec / 60) % 60 === 0
      && triggerConfig.timer_trigger.interval_sec / 60 / 60 > 0
    ) {
      setIntervalCaptureTimeUnit('hour');
      setIntervalCaptureTime(
        triggerConfig.timer_trigger.interval_sec / 60 / 60
      );
    } else {
      setIntervalCaptureTimeUnit('minute');
      setIntervalCaptureTime(triggerConfig.timer_trigger.interval_sec / 60);
    }

    // Handle case where one second is the base point
    triggerConfig.timer_trigger.time_node.map(item => {
      if (typeof item === 'number') {
        const hour = item / 60 / 60;
        const minute = (item / 60) % 60;
        return `${hour}:${minute}`;
      }
      return item;
    });

    // Initialize start_time for scheduled interval mode
    if (triggerConfig.timer_trigger.start_time) {
      const st = triggerConfig.timer_trigger.start_time;
      if (typeof st === 'string' && st.includes(':')) {
        setScheduledStartTime(st);
      }
    }

    // Initialize end_time (device always emits it; '23:59' fallback is only
    // for old firmware that omitted the field when it was 0)
    setScheduledEndTime(triggerConfig.timer_trigger.end_time || '23:59');

    // Initialize the anchor "HH:MM" from the device; when never stamped,
    // prefill with the current time (matches the device's stamp-on-apply)
    const { anchor } = triggerConfig.timer_trigger;
    setAnchorInput(anchor && anchor.includes(':') ? anchor : toHHMM(new Date()));
  };
  useEffect(() => {
    initIntervalCaptureTime();
    // Re-sync on the whole timer_trigger object, not just interval_sec: a
    // refreshed config that keeps the same interval but changes
    // start/end/anchor must still re-populate the inputs
  }, [triggerConfig.timer_trigger]);

  // Interval is a daily-lattice step — integers only (decimals are rounded at
  // commit, never mid-typing), and it must stay under 24h (hour ≤ 23,
  // minute ≤ 1439)
  const intervalMax = intervalCaptureTimeUnit === 'hour' ? 23 : 1439;
  const clampInterval = (value: number) => {
    const rounded = Math.round(Number.isNaN(value) ? 1 : value);
    return Math.max(1, Math.min(intervalMax, rounded));
  };

  const handleIntervalCaptureTimeChange = (e: Event) => {
    // Keep any intermediate text (including "") as the draft; parse + clamp
    // once on blur
    setIntervalDraft((e.target as HTMLInputElement).value);
  };
  const handleIntervalCaptureTimeUnitChange = (value: string) => {
    setIntervalCaptureTimeUnit(value);
    // Re-clamp for the new unit (e.g. 120 minutes -> 23 hours)
    setIntervalCaptureTime(prev => Math.max(1, Math.min(value === 'hour' ? 23 : 1439, prev)));
  };
  const handleAddIntervalCapture = () => {
    if (!triggerConfig.timer_trigger) return;
    const newWeekdays = [...(triggerConfig.timer_trigger.weekdays || []), 0];
    const newTimeNode = [...triggerConfig.timer_trigger.time_node, '00:00'];
    setTriggerConfig({
      ...triggerConfig,
      timer_trigger: {
        ...triggerConfig.timer_trigger,
        time_node_count: newTimeNode.length,
        time_node: newTimeNode,
        weekdays: newWeekdays,
      },
    });
  };
  const handleCaptureTimeChange = (index: number, value: string) => {
    if (!triggerConfig.timer_trigger) return;
    let weekUnit = value.split(' ')[0];
    const timeStr = value.split(' ')[1];
    if (Number.isNaN(Number(weekUnit))) {
      weekUnit = Number(
        Array.from(WeekUnitMap.entries()).find(
          item => item[1] === weekUnit
        )?.[0]
      ) as unknown as string;
    }
    const newWeekdays = triggerConfig.timer_trigger.weekdays.map((item, i) => (i === index ? Number(weekUnit) : item));
    const newTimeNode = triggerConfig.timer_trigger.time_node.map((item, i) => (i === index ? timeStr : item));
    setTriggerConfig({
      ...triggerConfig,
      timer_trigger: {
        ...triggerConfig.timer_trigger,
        time_node_count: newTimeNode.length,
        time_node: newTimeNode,
        weekdays: newWeekdays,
      },
    });
  };

  const handleIntervalCapture = async () => {
    if (!triggerConfig.timer_trigger) return;
    const formateTime = intervalCaptureTimeUnit === 'hour'
      ? intervalCaptureTime * 60 * 60
      : intervalCaptureTime * 60;
    if (formateTime < 60 || formateTime >= 86400) {
      // Daily-lattice step must be a strict sub-day amount
      toast.error(i18n._('sys.device_tool.interval_limit_error'));
      return;
    }
    const isScheduled =      triggerConfig.timer_trigger.interval_mode === 'scheduled';
    if (isScheduled) {
      const startSec = hhmmToSec(scheduledStartTime);
      const endSec = hhmmToSec(scheduledEndTime || '23:59');
      // Equal start/end is reserved for the normal-mode lattice's internal
      // representation; a scheduled full day is any start T with end T-1min
      // (00:00-23:59 is just the midnight-anchored case)
      if (endSec === startSec) {
        toast.error(i18n._('sys.device_tool.end_eq_start_error'));
        return;
      }
      // Inclusive window: closed [start, end] — span + 1 so a node landing
      // exactly on the end fires
      const windowSec =        (endSec > startSec
          ? endSec - startSec
          : endSec + 86400 - startSec) + 1;
      if (windowSec <= formateTime) {
        toast.error(i18n._('sys.device_tool.window_le_interval_error'));
        return;
      }
    }
    try {
      const tt = {
        ...triggerConfig.timer_trigger,
        interval_sec: formateTime,
        interval_mode: triggerConfig.timer_trigger.interval_mode || 'normal',
      };
      if (isScheduled) {
        // Only the scheduled editors are authoritative for the window.
        // Daily window end, closed [start, end]: 00:00 = ends at midnight;
        // must differ from start (a full day is any start T with end T-1min,
        // guarded below)
        tt.start_time = scheduledStartTime;
        tt.end_time = scheduledEndTime;
      } else {
        // Normal mode runs on `anchor` — start/end belong to the scheduled
        // window, so never overwrite them here: echo the stored values (the
        // spread above carries the GET's) or the configured window would be
        // silently reset to 00:00 and lost when switching back.
        // Daily grid anchor "HH:MM" (a time-of-day, no date component)
        tt.anchor = anchorInput;
      }
      await setTimerTriggerApi(tt);
      toast.success(i18n._('common.configSuccess'));
    } catch (error) {
      console.error('handleIntervalCapture', error);
      throw error;
    }
  };
  const handleFixedCapture = async () => {
    if (!triggerConfig.timer_trigger) return;
    try {
      await setTimerTriggerApi({ ...triggerConfig.timer_trigger, capture_mode: 'once' });
      toast.success(i18n._('common.configSuccess'));
    } catch (error) {
      console.error('handleFixedCapture', error);
      throw error;
    }
  };
  const setTriggerConfigApi = async (config = triggerConfig): Promise<boolean> => {
    let applyFailed = false;
    try {
      const res = await configTriggerConfigReq(config as unknown as PirConfigReq);
      // PIR chip-level write failure: config IS saved (retried at sleep/boot),
      // but the sensor didn't take it - warn the user instead of staying silent.
      if (res.data?.pir_apply_failed) {
        applyFailed = true;
        toast.warning(i18n._('sys.device_tool.pir_apply_failed'));
      }
      // Re-fetch from server to get computed fields like next_capture_at
      const res2 = await getTriggerConfigReq();
      setTriggerConfig(res2.data);
      setSavedNextCaptureAt(res2.data.timer_trigger?.next_capture_at ?? null);
    } catch (error) {
      console.error('setImageTrigger', error);
      throw error;
    }
    return applyFailed;
  };

  /* Timer / remote use the PARTIAL endpoints: they skip the chip-level PIR
   * write (a seconds-long retry loop with an absent sensor) that the combined
   * endpoint always runs. Same refetch afterwards for computed fields. */
  const setTimerTriggerApi = async (timer_trigger: NonNullable<TriggerConfigType['timer_trigger']>) => {
    try {
      await configTimerTriggerReq(timer_trigger);
      const res2 = await getTriggerConfigReq();
      setTriggerConfig(res2.data);
      setSavedNextCaptureAt(res2.data.timer_trigger?.next_capture_at ?? null);
    } catch (error) {
      console.error('setTimerTrigger', error);
      throw error;
    }
  };

  const setRemoteTriggerApi = async (enable: boolean) => {
    try {
      await configRemoteTriggerReq(enable);
      const res2 = await getTriggerConfigReq();
      setTriggerConfig(res2.data);
      setSavedNextCaptureAt(res2.data.timer_trigger?.next_capture_at ?? null);
    } catch (error) {
      console.error('setRemoteTrigger', error);
      throw error;
    }
  };

  const handlePirTriggerSave = async () => {
    try {
      setSavePirTriggerLoading(true);
      const newConfig = {
        ...triggerConfig,
        pir_trigger: { ...triggerConfig.pir_trigger },
      };
      const applyFailed = await setTriggerConfigApi(newConfig);
      if (!applyFailed) {
        toast.success(i18n._('common.configSuccess'));
      }
    } catch (error) {
      console.error('handlePirTriggerSave', error);
      throw error;
    } finally {
      setSavePirTriggerLoading(false);
    }
  };

  const handlePirTriggerChange = async (value: boolean) => {
    if (!triggerConfig.pir_trigger) return;
    const newConfig = {
      ...triggerConfig,
      pir_trigger: { ...triggerConfig.pir_trigger, enable: value },
    };
    setTriggerConfigApi(newConfig);
  };
  const handleTimerTriggerChange = (value: boolean) => {
    if (!triggerConfig.timer_trigger) return;
    setTimerTriggerApi({ ...triggerConfig.timer_trigger, enable: value });
  };
  const handleRemoteTriggerChange = (value: boolean) => {
    setRemoteTriggerApi(value);
  };

  // Live preview of the next capture node from the PENDING (edited) config
  // and the current time — a client-side port of the device's daily-lattice
  // math (shared by both interval modes: nodes start + k*interval inside
  // [start, start+window), the lattice restarts at start every day), so it
  // updates as the user edits the anchor/interval/window and as time passes,
  // instead of freezing at the last save. Falls back to the device-reported
  // value when inputs are not yet known.
  const computeNextCapturePreview = (): number | null => {
    const tt = triggerConfig.timer_trigger;
    if (!tt?.enable || tt.capture_mode !== 'interval') return savedNextCaptureAt;
    const intervalSec =      intervalCaptureTimeUnit === 'hour'
        ? intervalCaptureTime * 3600
        : intervalCaptureTime * 60;
    if (!intervalSec || intervalSec < 1) return savedNextCaptureAt;

    const isScheduled = tt.interval_mode === 'scheduled';
    const startSec = isScheduled
      ? hhmmToSec(scheduledStartTime)
      : hhmmToSec(anchorInput || tt.anchor || '00:00');
    // Mirror the device's interval_capture_window: normal mode is always a
    // full-day lattice, expressed as end == start
    const endSec = isScheduled
      ? hhmmToSec(scheduledEndTime || '23:59')
      : startSec;

    const d = new Date();
    const nowSec = d.getHours() * 3600 + d.getMinutes() * 60 + d.getSeconds();
    // Window: non-full-day is closed [start, end] (span + 1); equal = full day
    const windowSec =      endSec === startSec
        ? 86400
        : (endSec > startSec
            ? endSec - startSec
            : endSec + 86400 - startSec) + 1;
    const cyc = (nowSec + 86400 - startSec) % 86400;
    const nxt = cyc - (cyc % intervalSec) + intervalSec;
    // nxt >= windowSec -> window exhausted -> node is the next window start
    const nodeSec = (startSec + (nxt >= windowSec ? 0 : nxt)) % 86400;
    const dayOffset = nodeSec <= nowSec ? 1 : 0;
    const midnight = new Date(d);
    midnight.setHours(0, 0, 0, 0);
    return Math.floor(midnight.getTime() / 1000) + dayOffset * 86400 + nodeSec;
  };

  const intervalCapture = () => (
    <div className="">
      {/* Interval type selector */}
      <div className="flex justify-between items-center gap-2 mb-2">
        <Label className="text-sm text-text-primary">
          {i18n._('sys.device_tool.interval_type')}
        </Label>
        <div className="flex rounded-md border border-gray-200 overflow-hidden">
          <button
            className={`px-3 py-1.5 text-xs font-medium ${triggerConfig.timer_trigger?.interval_mode !== 'scheduled' ? 'bg-[#f24a00] text-white' : 'bg-white text-gray-500'}`}
            onClick={() => {
              setSavedNextCaptureAt(null);
              setTriggerConfig({
                ...triggerConfig,
                timer_trigger: {
                  ...triggerConfig.timer_trigger,
                  interval_mode: 'normal',
                },
              });
            }}
          >
            {i18n._('sys.device_tool.normal_interval')}
          </button>
          <button
            className={`px-3 py-1.5 text-xs font-medium ${triggerConfig.timer_trigger?.interval_mode === 'scheduled' ? 'bg-[#f24a00] text-white' : 'bg-white text-gray-500'}`}
            onClick={() => {
              setSavedNextCaptureAt(null);
              setTriggerConfig({
                ...triggerConfig,
                timer_trigger: {
                  ...triggerConfig.timer_trigger,
                  interval_mode: 'scheduled',
                },
              });
            }}
          >
            {i18n._('sys.device_tool.scheduled_interval')}
          </button>
        </div>
      </div>

      {/* Interval value */}
      <div className="flex justify-between items-center gap-2">
        <Label className="text-sm text-text-primary">
          {i18n._('sys.device_tool.interval_capture')}
        </Label>
        <div className="flex items-center">
          <Input
            type="number"
            min={1}
            max={intervalMax}
            className="w-20"
            value={intervalDraft ?? intervalCaptureTime}
            onChange={handleIntervalCaptureTimeChange}
            onBlur={e => {
              const clampedValue = clampInterval(
                Number((e.target as HTMLInputElement).value)
              );
              setIntervalCaptureTime(clampedValue);
              setIntervalDraft(null);
            }}
            onKeyDown={e => {
              // Enter commits (blurs) like clicking away
              if (e.key === 'Enter') {
                e.preventDefault();
                (e.target as HTMLInputElement).blur();
              }
            }}
          />
          <Select
            value={intervalCaptureTimeUnit}
            onValueChange={handleIntervalCaptureTimeUnitChange}
          >
            <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="hour">{i18n._('common.hour')}</SelectItem>
              <SelectItem value="minute">{i18n._('common.minute')}</SelectItem>
            </SelectContent>
          </Select>
        </div>
      </div>

      {/* Start / end time — only for scheduled mode */}
      {triggerConfig.timer_trigger?.interval_mode === 'scheduled' && (
        <>
          <div className="flex justify-between items-center gap-2 mt-2">
            <Label className="text-sm text-text-primary">
              {i18n._('sys.device_tool.start_time')}
            </Label>
            <TimePicker
              value={scheduledStartTime}
              onChange={(value: string) => setScheduledStartTime(value)}
              className="w-32"
            />
          </div>
          <div className="flex justify-between items-center gap-2 mt-2">
            <Label className="text-sm text-text-primary">
              {i18n._('sys.device_tool.end_time')}
            </Label>
            <TimePicker
              value={scheduledEndTime}
              onChange={(value: string) => setScheduledEndTime(value)}
              className="w-32"
            />
          </div>
          <p className="text-xs text-text-secondary mt-1 text-right">
            {i18n._('sys.device_tool.end_time_note')}
          </p>
        </>
      )}

      {/* Grid anchor — only for normal interval mode */}
      {triggerConfig.timer_trigger?.interval_mode !== 'scheduled' && (
        <>
          <div className="flex justify-between items-center gap-2 mt-2">
            <Label className="text-sm text-text-primary">
              {i18n._('sys.device_tool.anchor')}
            </Label>
            <TimePicker
              value={anchorInput}
              onChange={(value: string) => setAnchorInput(value)}
              className="w-32"
            />
          </div>
          <p className="text-xs text-text-secondary mt-1 text-right">
            {i18n._('sys.device_tool.anchor_note')}
          </p>
        </>
      )}

      {/* Next capture — live preview from the pending config + current time;
          falls back to the device-reported value until an anchor exists */}
      {(computeNextCapturePreview() ?? savedNextCaptureAt) != null
        && triggerConfig.timer_trigger?.enable && (
        <div className="mt-2 p-2 bg-orange-50 border border-orange-100 rounded-md text-xs text-orange-700 flex justify-between">
          <span>{i18n._('sys.device_tool.next_capture')}</span>
          <span className="font-medium">
            {new Date(
              (computeNextCapturePreview() ?? savedNextCaptureAt)! * 1000
            ).toLocaleString()}
          </span>
        </div>
      )}

      <div className="flex justify-end mt-2">
        <Button variant="primary" onClick={handleIntervalCapture}>
          {i18n._('common.confirm')}
        </Button>
      </div>
    </div>
  );

  const handleDeleteFixedCapture = (index: number) => {
    if (!triggerConfig.timer_trigger) return;
    const newTimeNode = triggerConfig.timer_trigger.time_node.filter(
      (_, i) => i !== index
    );
    setTriggerConfig({
      ...triggerConfig,
      timer_trigger: {
        ...triggerConfig.timer_trigger,
        time_node_count: newTimeNode.length,
        time_node: newTimeNode,
      },
    });
  };
  const customSlot = (cb: () => void) => (
    <Button variant="outline" size="sm" onClick={cb}>
      {i18n._('common.delete')}
    </Button>
  );

  const fixedCapture = () => (
    <div className="mt-2">
      <div className="flex justify-between items-center">
        <Label className="text-sm text-text-primary">
          {i18n._('sys.device_tool.capture_mode')}
        </Label>

        <Button
          disabled={(triggerConfig.timer_trigger?.time_node_count || 0) >= 10}
          variant="outline"
          onClick={handleAddIntervalCapture}
        >
          <SvgIcon icon="add" />
          {i18n._('common.add')}
        </Button>
      </div>
      {triggerConfig.timer_trigger?.time_node?.map((_, index) => (
        <div key={index}>
          <TimePicker
            showWeekSelect
            customSlot={() => customSlot(() => handleDeleteFixedCapture(index))}
            value={
              `${WeekUnitMap.get(triggerConfig.timer_trigger?.weekdays?.[index] as unknown as number) || `${i18n._('common.everyday')}`} ${triggerConfig.timer_trigger?.time_node?.[index]}`
              || 'Everyday 00:00'
            }
            className="w-full mt-2"
            onChange={value => handleCaptureTimeChange(index, value)}
          />
        </div>
      ))}
      {(triggerConfig.timer_trigger?.time_node_count || 0) > 0 && (
        <div className="flex justify-end mt-2">
          <Button variant="primary" onClick={handleFixedCapture}>
            {i18n._('common.confirm')}
          </Button>
        </div>
      )}
    </div>
  );

  return (
    <>
      {/* Trigger method */}
      <p className="text-sm text-text-primary mb-2 font-semibold">
        {i18n._('sys.device_tool.trigger')}
      </p>
      <Card className="bg-gray-50">
        <CardContent className="">
          <div ref={childeRef} className="w-full h-full ">
            <div className="flex items-center justify-between">
              <div className="flex items-center gap-2">
                <Label className="text-sm text-text-primary">
                  {' '}
                  {i18n._('sys.device_tool.trigger_pir')}
                </Label>
                <Tooltip mbEnhance>
                  <TooltipTrigger>
                    <div className="flex justify-center items-center">
                      <SvgIcon className="w-4 h-4" icon="info" />
                    </div>
                  </TooltipTrigger>
                  <TooltipContent className="max-w-80 text-pretty">
                    <p>{i18n._('sys.device_tool.pir_note')}</p>
                  </TooltipContent>
                </Tooltip>
              </div>
              <Switch
                checked={triggerConfig.pir_trigger?.enable || false}
                onCheckedChange={value => handlePirTriggerChange(value)}
                aria-label="Toggle theme"
                className="transition-all duration-700 ease-[cubic-bezier(0.34,1.56,0.64,1)] hover:scale-110"
              />
            </div>
            {triggerConfig.pir_trigger?.enable && (
              <div className="border border-gray-200 border-solid p-4 rounded-md mt-2">
                {PIRLoading ? (
                  <PIRSkeleton />
                ) : (
                  <>
                    {/* Device type on the wake input: PIR chip or plain IO */}
                    <div className="flex justify-between gap-2 flex-1 pr-0">
                      <Label className="text-sm text-text-primary shrink-0">
                        {i18n._('sys.device_tool.pir_device_type')}
                      </Label>
                      <Select
                        value={triggerConfig.pir_trigger?.device_type ?? 'pir'}
                        onValueChange={value => setTriggerConfig({
                          ...triggerConfig,
                          pir_trigger: {
                            ...triggerConfig.pir_trigger,
                            device_type: value as 'pir' | 'io',
                          },
                        })}
                      >
                        <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
                          <SelectValue
                            placeholder={i18n._('sys.device_tool.pir_device_type')}
                          />
                        </SelectTrigger>
                        <SelectContent>
                          <SelectItem value="pir">
                            {i18n._('sys.device_tool.pir_type_pir')}
                          </SelectItem>
                          <SelectItem value="io">
                            {i18n._('sys.device_tool.pir_type_io')}
                          </SelectItem>
                        </SelectContent>
                      </Select>
                    </div>
                    {/* Plain IO signal: only the wake edge matters */}
                    {(triggerConfig.pir_trigger?.device_type ?? 'pir') === 'io' && (
                      <div className="flex justify-between gap-2 flex-1 pr-0">
                        <Label className="text-sm text-text-primary shrink-0">
                          {i18n._('sys.device_tool.trigger_type')}
                        </Label>
                        <Select
                          value={
                            triggerConfig.pir_trigger?.trigger_type
                            || 'rising_edge'
                          }
                          onValueChange={value => setTriggerConfig({
                            ...triggerConfig,
                            pir_trigger: {
                              ...triggerConfig.pir_trigger,
                              trigger_type: value as
                                | 'rising_edge'
                                | 'falling_edge'
                                | 'both_edges'
                                | 'high_level'
                                | 'low_level',
                            },
                          })}
                        >
                          <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
                            <SelectValue
                              placeholder={i18n._('sys.device_tool.trigger_in')}
                            />
                          </SelectTrigger>
                          <SelectContent>
                            <SelectItem value="rising_edge">
                              {i18n._('sys.device_tool.rising_edge')}
                            </SelectItem>
                            <SelectItem value="falling_edge">
                              {i18n._('sys.device_tool.falling_edge')}
                            </SelectItem>
                          </SelectContent>
                        </Select>
                      </div>
                    )}
                    {(triggerConfig.pir_trigger?.device_type ?? 'pir') !== 'io' && (
                      <>
                        {/* Quick recommended presets — label left, chips right,
                         * vertically centered like the other rows */}
                        <div className="flex justify-between items-center gap-2 flex-1 pr-0 my-4">
                          <Label className="text-sm text-text-primary shrink-0">
                            {i18n._('sys.device_tool.pir_presets')}
                          </Label>
                          <div className="flex flex-wrap gap-2 justify-end">
                            {PIR_PRESETS.map(preset => {
                              const pir = triggerConfig.pir_trigger;
                              const active = pir
                                && pir.sensitivity_level === preset.sens
                                && pir.pulse_count === preset.pulse
                                && PIR_WINDOW_SECONDS[pir.window_time_s ?? 0] === preset.window
                                && pirIgnoreSeconds(pir.ignore_time_s ?? 0) === preset.ignore;
                              return (
                                <Button
                                  key={preset.key}
                                  className="h-7 px-2 text-xs"
                                  variant={active ? 'primary' : 'outline'}
                                  onClick={() => setTriggerConfig({
                                    ...triggerConfig,
                                    pir_trigger: {
                                      ...triggerConfig.pir_trigger,
                                      sensitivity_level: preset.sens,
                                      pulse_count: preset.pulse,
                                      window_time_s: (preset.window - 2) / 2,
                                      ignore_time_s: (preset.ignore - 0.5) / 0.5,
                                    },
                                  })}
                                >
                                  {i18n._(`sys.device_tool.pir_preset_${preset.key}`)}
                                </Button>
                              );
                            })}
                          </div>
                        </div>
                        {/* Sensitivity: UI 1-100 mapped onto register 120-20 */}
                        <div className="w-full flex flex-col gap-1 flex-1 pr-0 my-4">
                          <div className="flex items-center justify-between gap-4">
                            <Label className="text-sm text-text-primary shrink-0">
                              {i18n._('sys.device_tool.sensitivity_level')}
                            </Label>
                            <div className="flex items-center gap-2 flex-1">
                              <Slider
                                className="flex-1 min-w-16"
                                value={sensRegToUi(triggerConfig.pir_trigger?.sensitivity_level ?? 30)}
                                max={100}
                                step={1}
                                min={1}
                                onChange={value => setTriggerConfig({
                                  ...triggerConfig,
                                  pir_trigger: {
                                    ...triggerConfig.pir_trigger,
                                    sensitivity_level: sensUiToReg(value),
                                  },
                                })}
                              />
                              {/* Draft-based number input: the 1s now-tick re-renders
                                  every second, so a blur-only controlled input would
                                  snap back to the committed value mid-typing */}
                              <div className="flex items-center gap-1">
                                <NumberField
                                  className="w-16 text-right"
                                  min={1}
                                  max={100}
                                  value={sensRegToUi(triggerConfig.pir_trigger?.sensitivity_level ?? 30)}
                                  onCommit={v => setTriggerConfig({
                                    ...triggerConfig,
                                    pir_trigger: {
                                      ...triggerConfig.pir_trigger,
                                      sensitivity_level: sensUiToReg(v),
                                    },
                                  })}
                                />
                                <span className="text-sm text-text-secondary">%</span>
                              </div>
                            </div>
                          </div>
                          {/* Tier + estimated range + live SENS register hint */}
                          {(() => {
                            const reg = sensClamp(triggerConfig.pir_trigger?.sensitivity_level ?? 30);
                            const uiPct = sensRegToUi(reg);
                            return (
                              <p className="text-xs text-text-secondary self-end flex items-center gap-2">
                                <span>
                                  {i18n._(sensTierKey(uiPct))}
                                  {' · '}
                                  {i18n._('sys.device_tool.sens_est_dist').replace('{dist}', sensEstDistance(uiPct).toFixed(1))}
                                </span>
                                <span className="text-gray-400 font-mono">
                                  {i18n._('sys.device_tool.sens_register_value').replace('{reg}', String(reg))}
                                </span>
                              </p>
                            );
                          })()}
                        </div>
                    {/* Anti-jamming: N valid pulses within the window trigger a wake.
                        Units stay OUTSIDE the dropdowns (bare numbers inside) so
                        the selects stay narrow and the sentence flows. Label on
                        the left like the presets row; the config cluster wraps
                        within the right side (English no longer pushes values
                        to a misaligned far-right edge). */}
                    <div className="flex flex-col sm:flex-row sm:justify-between gap-2 flex-1 pr-0 my-4">
                      <Label className="text-sm text-text-primary shrink-0">
                        {i18n._('sys.device_tool.anti_jamming')}
                      </Label>
                      <div className="flex flex-wrap items-center gap-x-2 gap-y-0 justify-start sm:justify-end text-sm text-text-secondary">
                        <span>{i18n._('sys.device_tool.anti_within')}</span>
                        <Select
                          value={String(PIR_WINDOW_SECONDS[triggerConfig.pir_trigger?.window_time_s ?? 0])}
                          onValueChange={value => setTriggerConfig({
                            ...triggerConfig,
                            pir_trigger: {
                              ...triggerConfig.pir_trigger,
                              window_time_s: (Number(value) - 2) / 2,
                            },
                          })}
                        >
                          <SelectTrigger size="sm" className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent w-11 px-1 data-[size=sm]:h-5">
                            <SelectValue />
                          </SelectTrigger>
                          <SelectContent>
                            {PIR_WINDOW_SECONDS.map(sec => (
                              <SelectItem key={sec} value={String(sec)}>
                                {sec}
                              </SelectItem>
                            ))}
                          </SelectContent>
                        </Select>
                        <span>{i18n._('sys.device_tool.anti_seconds_unit')}</span>
                        <span>{i18n._('sys.device_tool.anti_detect')}</span>
                        <Select
                          value={String(triggerConfig.pir_trigger?.pulse_count ?? 1)}
                          onValueChange={value => setTriggerConfig({
                            ...triggerConfig,
                            pir_trigger: {
                              ...triggerConfig.pir_trigger,
                              pulse_count: Number(value),
                            },
                          })}
                        >
                          <SelectTrigger size="sm" className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent w-11 px-1 data-[size=sm]:h-5">
                            <SelectValue />
                          </SelectTrigger>
                          <SelectContent>
                            {[1, 2, 3, 4].map(n => (
                              <SelectItem key={n} value={String(n)}>
                                {n}
                              </SelectItem>
                            ))}
                          </SelectContent>
                        </Select>
                        <span>{i18n._('sys.device_tool.anti_trigger')}</span>
                      </div>
                    </div>
                    {/* Cooldown (ignore time): pulled out of the advanced
                     * section — it pairs directly with the trigger rhythm
                     * above and is tuned per site. */}
                    <div className="flex justify-between gap-2 flex-1 pr-0 my-4">
                      <div className="flex items-center gap-2">
                        <Label className="text-sm text-text-primary shrink-0">
                          {i18n._('sys.device_tool.ignore_time')}
                        </Label>
                        <Tooltip mbEnhance>
                          <TooltipTrigger>
                            <div className="w-4 flex justify-center items-center">
                              <SvgIcon className="w-4 h-4" icon="info" />
                            </div>
                          </TooltipTrigger>
                          <TooltipContent className="max-w-80 text-pretty">
                            <p>{i18n._('sys.device_tool.ignore_time_note')}</p>
                          </TooltipContent>
                        </Tooltip>
                      </div>
                      <Select
                        value={String(triggerConfig.pir_trigger?.ignore_time_s ?? 7)}
                        onValueChange={value => setTriggerConfig({
                          ...triggerConfig,
                          pir_trigger: {
                            ...triggerConfig.pir_trigger,
                            ignore_time_s: Number(value),
                          },
                        })}
                      >
                        <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
                          <SelectValue
                            placeholder={i18n._('sys.device_tool.trigger_in')}
                          />
                        </SelectTrigger>
                        <SelectContent>
                          {Array.from({ length: 16 }).map((_, index) => (
                            <SelectItem key={index} value={index.toString()}>
                              {pirIgnoreSeconds(index).toFixed(1)}{i18n._('sys.device_tool.anti_seconds_unit')}
                            </SelectItem>
                          ))}
                        </SelectContent>
                      </Select>
                    </div>
                    {/* Advanced PIR options (SENS register + disable-in-preview)
                     * hidden for now — re-enable when field tuning needs them.
                    <div className="flex flex-col gap-2">
                      <button
                        type="button"
                        className="flex items-center gap-1 text-sm text-text-secondary self-start"
                        onClick={() => setPirAdvancedOpen(open => !open)}
                      >
                        <SvgIcon
                          icon="right"
                          className={`h-4 w-4 transition-transform ${pirAdvancedOpen ? 'rotate-90' : ''}`}
                        />
                        {i18n._('sys.device_tool.advanced_options')}
                      </button>
                      {pirAdvancedOpen && (
                        <div className="border border-gray-200 border-solid p-3 rounded-md flex flex-col gap-3">
                          <div className="flex justify-between gap-2 flex-1 pr-0">
                            <Label className="text-sm text-text-primary shrink-0">
                              {i18n._('sys.device_tool.sens_register')}
                            </Label>
                            <NumberField
                              className="w-20 text-right"
                              min={10}
                              max={255}
                              value={triggerConfig.pir_trigger?.sensitivity_level ?? 30}
                              onCommit={v => setTriggerConfig({
                                ...triggerConfig,
                                pir_trigger: {
                                  ...triggerConfig.pir_trigger,
                                  sensitivity_level: v,
                                },
                              })}
                            />
                          </div>
                          <div className="flex justify-between gap-2 flex-1 pr-0">
                            <div className="flex items-center gap-2">
                              <Label className="text-sm text-text-primary shrink-0">
                                {i18n._('sys.device_tool.disable_pir_in_preview')}
                              </Label>
                              <Tooltip mbEnhance>
                                <TooltipTrigger>
                                  <div className="w-4 flex justify-center items-center">
                                    <SvgIcon className="w-4 h-4" icon="info" />
                                  </div>
                                </TooltipTrigger>
                                <TooltipContent className="max-w-80 text-pretty">
                                  <p>{i18n._('sys.device_tool.disable_pir_in_preview_note')}</p>
                                </TooltipContent>
                              </Tooltip>
                            </div>
                            <Select
                              value={
                                (triggerConfig.pir_trigger?.disable_in_preview ?? false)
                                  ? 'yes'
                                  : 'no'
                              }
                              onValueChange={value => setTriggerConfig({
                                ...triggerConfig,
                                pir_trigger: {
                                  ...triggerConfig.pir_trigger,
                                  disable_in_preview: value === 'yes',
                                },
                              })}
                            >
                              <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
                                <SelectValue
                                  placeholder={i18n._('sys.device_tool.trigger_in')}
                                />
                              </SelectTrigger>
                              <SelectContent>
                                <SelectItem value="yes">
                                  {i18n._('common.yes')}
                                </SelectItem>
                                <SelectItem value="no">
                                  {i18n._('common.no')}
                                </SelectItem>
                              </SelectContent>
                            </Select>
                          </div>
                        </div>
                      )}
                    </div>
                    */}
                      </>
                    )}
                    <div className="flex justify-end mt-2">
                      <Button
                        variant="primary"
                        disabled={savePirTriggerLoading}
                        onClick={handlePirTriggerSave}
                      >
                        {savePirTriggerLoading ? (
                          <div className="w-full h-full flex items-center justify-center">
                            <div
                              className="w-4 h-4 rounded-full border-2 border-[#f24a00] border-t-transparent animate-spin"
                              aria-label="loading"
                            />
                          </div>
                        ) : (
                          i18n._('common.save')
                        )}
                      </Button>
                    </div>
                  </>
                )}
              </div>
            )}
            <Separator className="my-2" />
            <div className="">
              <div className="flex items-center  justify-between">
                <div className="flex items-center gap-2">
                  <Label className="text-sm text-text-primary">
                    {' '}
                    {i18n._('sys.device_tool.remote_control')}
                  </Label>
                  <Tooltip mbEnhance>
                    <TooltipTrigger>
                      <div className="w-4 flex justify-center items-center">
                        <SvgIcon className="w-4 h-4" icon="info" />
                      </div>
                    </TooltipTrigger>
                    <TooltipContent className="max-w-80 text-pretty">
                      <p>{i18n._('sys.device_tool.remote_control_note')}</p>
                    </TooltipContent>
                  </Tooltip>
                </div>
                <Switch
                  checked={triggerConfig.remote_trigger?.enable || false}
                  onCheckedChange={value => handleRemoteTriggerChange(value)}
                  aria-label="Toggle theme"
                  className="transition-all duration-700 ease-[cubic-bezier(0.34,1.56,0.64,1)] hover:scale-110"
                />
              </div>
            </div>
            <Separator className="my-2" />
            <div className="">
              <div className="flex items-center  justify-between">
                <div className="flex items-center gap-2">
                  <Label className="text-sm text-text-primary">
                    {' '}
                    {i18n._('sys.device_tool.schedule')}
                  </Label>
                  <Tooltip mbEnhance>
                    <TooltipTrigger>
                      <div className="w-4 flex justify-center items-center">
                        <SvgIcon className="w-4 h-4" icon="info" />
                      </div>
                    </TooltipTrigger>
                    <TooltipContent className="max-w-80 text-pretty">
                      <p>{i18n._('sys.device_tool.timing_capture_note')}</p>
                    </TooltipContent>
                  </Tooltip>
                </div>
                <Switch
                  checked={triggerConfig.timer_trigger?.enable || false}
                  onCheckedChange={value => handleTimerTriggerChange(value)}
                  aria-label="Toggle theme"
                  className="transition-all duration-700 ease-[cubic-bezier(0.34,1.56,0.64,1)] hover:scale-110"
                />
              </div>
            </div>
            {triggerConfig.timer_trigger?.enable && (
              <div className="border border-gray-200 border-solid p-4 rounded-md mt-2">
                <div className="flex items-center  justify-between">
                  <Label className="text-sm text-text-primary">
                    {i18n._('sys.device_tool.capture_mode')}
                  </Label>
                  <Select
                    value={
                      triggerConfig.timer_trigger?.capture_mode || 'interval'
                    }
                    onValueChange={value => setTriggerConfig({
                      ...triggerConfig,
                      timer_trigger: {
                        ...triggerConfig.timer_trigger,
                        capture_mode: value,
                      },
                    })}
                  >
                    <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
                      <SelectValue
                        placeholder={i18n._('sys.device_tool.trigger_in')}
                      />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="interval">
                        {i18n._('sys.device_tool.interval_capture')}
                      </SelectItem>
                      <SelectItem value="once">
                        {i18n._('sys.device_tool.fixed_capture')}
                      </SelectItem>
                    </SelectContent>
                  </Select>
                </div>
                {/* Mode parameter options */}
                {triggerConfig.timer_trigger?.capture_mode === 'interval'
                  && intervalCapture()}
                {triggerConfig.timer_trigger?.capture_mode === 'once'
                  && fixedCapture()}
              </div>
            )}
          </div>
        </CardContent>
      </Card>
    </>
  );
}
