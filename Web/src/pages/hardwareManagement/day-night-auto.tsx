import { useEffect, useState } from 'react';
import { useLingui } from '@lingui/react';
import { Label } from '@/components/ui/label';
import { Input } from '@/components/ui/input';
import { Separator } from '@/components/ui/separator';
import TimePicker from '@/components/time-picker';
import SvgIcon from '@/components/svg-icon';
import hardwareServiceApi, { type DayNightConfig } from '@/services/api/hardware-management';

const DEFAULT_CONFIG: DayNightConfig = {
  mode: 'auto',
  auto_source: 'time',
  custom_schedule: {
    start_hour: 18,
    start_minute: 0,
    end_hour: 6,
    end_minute: 0,
  },
  light_sensor: {
    night_threshold: 20,
    day_threshold: 80,
  },
  isp_stats: {
    night_enter_lux: 50,
    day_enter_lux: 150,
    night_enter_exposure_us: 20000,
    night_enter_gain_mdb: 6000,
    night_enter_avgl: 70,
    day_enter_exposure_us: 8000,
    day_enter_gain_mdb: 3000,
    day_enter_avgl: 80,
    ema_alpha_num: 1,
    ema_alpha_den: 4,
  },
  ir_brightness: 50,
};

const pad2 = (n: number) => n.toString().padStart(2, '0');

/**
 * Live ambient-light percentage: polls the day/night status once per second.
 * It is ONLY mounted while the "adaptive by brightness" panel is visible —
 * switching to another switch logic or leaving the page unmounts this
 * component, and the effect cleanup stops the interval (the requirement is
 * that the device is not polled once the brightness UI is gone).
 */
function LiveBrightness() {
  const { i18n } = useLingui();
  const [percent, setPercent] = useState<number | null>(null);
  const [stale, setStale] = useState(false);

  useEffect(() => {
    let alive = true;
    // Ambient brightness changes slowly: 2 s is plenty for a live readout,
    // and setState fires ONLY when the value actually changes - a poll that
    // re-renders the panel every second visibly hiccuped the preview
    // running in another tab of the same browser.
    let last: number | null = null;
    const fetchIt = async () => {
      try {
        const res = await hardwareServiceApi.getDayNightStatusReq();
        const p = res.data?.metrics?.light_sensor_percent;
        if (!alive) return;
        const v = typeof p === 'number' ? p : null;
        if (v !== last) {
          last = v;
          setPercent(v);
        }
        setStale(prev => (prev ? false : prev));
      } catch {
        if (alive) setStale(prev => (prev || true));
      }
    };
    fetchIt();
    const timer = setInterval(fetchIt, 2000);
    return () => { alive = false; clearInterval(timer); };
  }, []);

  return (
    <div className="flex justify-between gap-4 items-center">
      <Label>{i18n._('sys.hardware_management.ls_current_value')}</Label>
      <span className={`text-sm min-w-[3rem] text-right ${stale ? 'text-gray-400' : 'text-gray-700'}`}>
        {percent != null ? `${percent}%` : '--'}
      </span>
    </div>
  );
}

/**
 * Sub-panel for the IR-board "day/night switch logic" selector:
 *  - schedule  : custom time window (inside the window -> NIGHT / IR mode)
 *  - adaptive  : ambient-light auto switching (live value + thresholds)
 * The panel is rendered only for those two logic values; day/night constants
 * need no extra configuration.
 */
export default function DayNightLogicConfig({ logic }: { logic: 'schedule' | 'adaptive' }) {
  const { i18n } = useLingui();
  const { getDayNightConfigReq, setDayNightConfigReq } = hardwareServiceApi;
  const [cfg, setCfg] = useState<DayNightConfig>(DEFAULT_CONFIG);
  const [startTime, setStartTime] = useState('18:00');
  const [endTime, setEndTime] = useState('06:00');

  useEffect(() => {
    let alive = true;
    (async () => {
      try {
        const res = await getDayNightConfigReq();
        if (!alive) return;
        const data = res.data ?? DEFAULT_CONFIG;
        setCfg(data);
        setStartTime(`${pad2(data.custom_schedule.start_hour)}:${pad2(data.custom_schedule.start_minute)}`);
        setEndTime(`${pad2(data.custom_schedule.end_hour)}:${pad2(data.custom_schedule.end_minute)}`);
      } catch (e) {
        console.error('initDayNight', e);
      }
    })();
    return () => { alive = false; };
  }, []);

  const patch = async (p: Partial<DayNightConfig>) => {
    const next = { ...cfg, ...p };
    setCfg(next);
    try {
      await setDayNightConfigReq(p);
    } catch (e) {
      console.error('setDayNight', e);
    }
  };

  const setSchedule = (which: 'start' | 'end', value: string) => {
    const [h, m] = value.split(':').map(Number);
    const sched = { ...cfg.custom_schedule };
    if (which === 'start') {
      sched.start_hour = h; sched.start_minute = m; setStartTime(value);
    } else {
      sched.end_hour = h; sched.end_minute = m; setEndTime(value);
    }
    patch({ custom_schedule: sched });
  };

  const setLsThreshold = (key: 'night_threshold' | 'day_threshold', v: number) => {
    patch({ light_sensor: { ...cfg.light_sensor, [key]: v } });
  };

  if (logic === 'schedule') {
    return (
      <>
        <Separator />
        <div className="flex md:flex-row flex-col md:gap-12 gap-2 justify-between">
          <Label className="shrink-0">{i18n._('sys.hardware_management.time_range')}</Label>
          <div className="flex items-center gap-2">
            <TimePicker value={startTime} className="flex-1 text-sm" onChange={v => setSchedule('start', v)} />
            <span className="text-gray-500">-</span>
            <TimePicker value={endTime} className="flex-1" onChange={v => setSchedule('end', v)} />
          </div>
        </div>
        {/* Semantics annotation: dn_eval_time() switches to NIGHT inside the
         * window and DAY outside of it — make that explicit to the user. */}
        <div className="flex items-start gap-1 rounded-md bg-amber-50 p-2 text-xs text-amber-700">
          <SvgIcon className="h-4 w-4 shrink-0 mt-0.5" icon="info" />
          <span>{i18n._('sys.hardware_management.dn_schedule_note')}</span>
        </div>
      </>
    );
  }

  /* logic === 'adaptive' */
  return (
    <>
      <Separator />
      <LiveBrightness />
      <Separator />
      <div className="flex justify-between gap-4 items-center">
        <Label className="shrink-0">{i18n._('sys.hardware_management.ls_night_threshold')}</Label>
        <Input
          className="w-24 text-right"
          type="number"
          min={0}
          max={100}
          value={cfg.light_sensor.night_threshold}
          onChange={e => setLsThreshold('night_threshold', Number((e.target as HTMLInputElement).value))}
          onBlur={e => setLsThreshold('night_threshold', Math.max(0, Math.min(100, Number((e.target as HTMLInputElement).value) || 0)))}
        />
      </div>
      <Separator />
      <div className="flex justify-between gap-4 items-center">
        <Label className="shrink-0">{i18n._('sys.hardware_management.ls_day_threshold')}</Label>
        <Input
          className="w-24 text-right"
          type="number"
          min={0}
          max={100}
          value={cfg.light_sensor.day_threshold}
          onChange={e => setLsThreshold('day_threshold', Number((e.target as HTMLInputElement).value))}
          onBlur={e => setLsThreshold('day_threshold', Math.max(0, Math.min(100, Number((e.target as HTMLInputElement).value) || 0)))}
        />
      </div>
    </>
  );
}
