import { useState, useRef, useEffect } from 'preact/hooks';
import { useLingui } from '@lingui/react';
import { Separator } from '@/components/ui/separator';
import { Card, CardContent } from '@/components/ui/card';
import { Tooltip, TooltipContent, TooltipTrigger } from '@/components/tooltip';
import { Button } from '@/components/ui/button';
import Upload from '@/components/upload';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';

import SvgIcon from '@/components/svg-icon';
import { Input } from '@/components/ui/input';
import { NumberField } from '@/components/number-field';
import Player from './player';
import ToolGuide from './tool-guide';
import TriggerConfig from './trigger-config';
import { getItem } from '@/utils/storage';
import { useNavigate } from 'react-router-dom';

import deviceTool, {
  type AiParams,
  type StreamConfigReq,
  type StreamResolution,
  type RuntimeCaptureConfig,
} from '@/services/api/deviceTool';
import hardwareManagement from '@/services/api/hardware-management';
import systemApis from '@/services/api/system';

import DeviceToolSkeleton from './skeleton';
import { useAiStatusStore } from '@/store/aiStatus';
import Slider from '@/components/slider';
import { Label } from '@/components/ui/label';
import { getWebSocketUrl, sliceFile } from '@/utils';
import WifiReloadMask from '@/components/wifi-reload-mask';
import H264Player from '@/lib/MSE/h264Player';
import RtmpConfig from './rtmp-config';
import { toast } from 'sonner';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogFooter,
  DialogDescription,
} from '@/components/dialog';

export interface ToolGuideContent {
  title: string;
  description: string;
  footer: string;
}
export interface ToolGuideProps {
  refList: React.RefObject<HTMLDivElement>[];
  scollRef: React.RefObject<HTMLDivElement>;
  isLoading: boolean;
  onClose?: () => void;
}

export default function DeviceTool() {
  const { i18n } = useLingui();
  const navigate = useNavigate();

  // eslint-disable-next-line @typescript-eslint/no-unused-vars
  // configImageTrigger
  const {
    getAiStatusReq,
    getPowerModeReq,
    switchPowerModeReq,
    getStreamConfigReq,
    setStreamConfigReq,
    getRuntimeCaptureReq,
    setRuntimeCaptureReq,
    getAiParamsReq,
    setAiParamsReq,
    stopVideoStreamReq,
    toggleAiReq,
  } = deviceTool;
  const { getSysClkConfigReq, setSysClkConfigReq } = hardwareManagement;
  const videoRendererInstance = useRef<H264Player | null>(null);
  const { uploadOTAFileReq, reloadModelReq, preCheckReq } = systemApis;
  const { setIsAiInference, setAiStatus, aiStatus } = useAiStatusStore();
  const [curPowerModel, setCurPowerModel] = useState<string>('full_speed');
  const handlePowerModelChange = async (value: string) => {
    try {
      await switchPowerModeReq({ mode: value as 'full_speed' | 'low_power' });
      setCurPowerModel(value);
    } catch (error) {
      console.error('handlePowerModelChange', error);
      throw error;
    }
  };
  // const [uploadLoading, setUploadLoading] = useState(false);
  const [modelUploadLoading, setModelUploadLoading] = useState(false);
  const [playerEpoch, setPlayerEpoch] = useState(0);
  const [sysClkProfile, setSysClkProfile] = useState<1 | 2 | 3 | 4>(4);
  const [sysClkFlashValid, setSysClkFlashValid] = useState(false);

  // Live video stream parameters (resolution / bitrate / keyframe interval)
  const [streamCfg, setStreamCfg] = useState<StreamConfigReq>({
    resolution: '720p',
    bitrate_kbps: 2000,
    gop_sec: 1,
  });
  const [streamSaving, setStreamSaving] = useState(false);
  // Resolution the device last confirmed (load or save): gates the pre-save player unmount
  const appliedResolutionRef = useRef<StreamConfigReq['resolution']>('720p');
  const loadStreamConfig = async () => {
    try {
      const res = await getStreamConfigReq();
      setStreamCfg({
        resolution: res.data.resolution ?? '720p',
        bitrate_kbps: res.data.bitrate_kbps ?? 2000,
        gop_sec: res.data.gop_sec ?? 1,
      });
      appliedResolutionRef.current = res.data.resolution ?? '720p';
    } catch (error) {
      console.error('loadStreamConfig', error);
      throw error;
    }
  };
  const handleStreamSave = async () => {
    try {
      setStreamSaving(true);
      await setStreamConfigReq(streamCfg);
      appliedResolutionRef.current = streamCfg.resolution;
      /* The device restarts its encoder session on apply and the WS server
       * closes clients with 4001 stream-reset on the fresh SPS; the player
       * worker reconnects and re-initializes from the new join-sync frame.
       * Remount once here as well so the MSE is rebuilt cleanly. Do NOT
       * unmount before the request: the brief hub-unsubscribe used to stop
       * the pipeline (a PIPE2 stop takes ~6s) and the reconnect then had to
       * rebuild it, oscillating in restarts that left the preview crawling. */
      setPlayerEpoch(epoch => epoch + 1);
      toast.success(i18n._('sys.device_tool.stream_saved'));
    } catch (error) {
      console.error('handleStreamSave', error);
      throw error;
    } finally {
      setStreamSaving(false);
    }
  };

  const [aiModelName, setAiModelName] = useState('');
  const [aiParams, setAiParams] = useState({
    nms_threshold: 0,
    confidence_threshold: 0,
  });

  // Runtime capture parameters (skip frames / jpeg quality). Capture
  // RESOLUTION is not stored here: it follows the live stream config
  // (pipe2 stream => 4MP pipe1 capture geometry, 720p/1080p likewise), so
  // the Select below is derived from streamCfg and changing it goes through
  // a confirm dialog that switches the stream too.
  const [runtimeCap, setRuntimeCap] = useState<RuntimeCaptureConfig>({
    runtime_capture_skip_frames: 0,
    runtime_capture_jpeg_quality: 60,
    ai_capture_enable: true,
  });
  const [runtimeCapSaving, setRuntimeCapSaving] = useState(false);
  const loadRuntimeCapture = async () => {
    try {
      const res = await getRuntimeCaptureReq();
      const ic = res.data.image_config;
      setRuntimeCap({
        runtime_capture_skip_frames: ic?.runtime_capture_skip_frames ?? 0,
        runtime_capture_jpeg_quality: ic?.runtime_capture_jpeg_quality ?? 60,
        ai_capture_enable: ic?.ai_capture_enable !== false,
      });
    } catch (error) {
      console.error('loadRuntimeCapture', error);
    }
  };
  const handleRuntimeCapSave = async () => {
    try {
      setRuntimeCapSaving(true);
      await setRuntimeCaptureReq({
        runtime_capture_skip_frames: runtimeCap.runtime_capture_skip_frames,
        runtime_capture_jpeg_quality: runtimeCap.runtime_capture_jpeg_quality,
      });
      toast.success(i18n._('sys.device_tool.runtime_capture_saved'));
    } catch (error) {
      console.error('handleRuntimeCapSave', error);
      throw error;
    } finally {
      setRuntimeCapSaving(false);
    }
  };

  // Capture-resolution Select -> confirm dialog -> stream switch. Capture
  // values map 1:1 onto stream values (4MP = the pipe2 sub-stream).
  type CaptureRes = '1080p' | '720p' | '4mp';
  const captureResFromStream: CaptureRes =    streamCfg.resolution === 'pipe2' ? '4mp' : streamCfg.resolution;
  const [capResDialogOpen, setCapResDialogOpen] = useState(false);
  const [pendingCapRes, setPendingCapRes] = useState<CaptureRes | null>(null);
  const capResLabel = (v: CaptureRes) => (v === '4mp' ? '4MP' : v === '720p' ? '720P' : '1080P');
  /* Dialog wording: at 4MP the stream becomes the sub-stream - say the
   * stream name only (no 4MP suffix), matching the stream params section. */
  const capResDialogLabel = (v: CaptureRes) => (v === '4mp' ? i18n._('sys.device_tool.stream_res_pipe2') : capResLabel(v));
  const handleCapResChange = (value: string) => {
    if (value === captureResFromStream) return;
    setPendingCapRes(value as CaptureRes);
    setCapResDialogOpen(true);
  };
  const handleCapResConfirm = async () => {
    if (!pendingCapRes) return;
    const newStreamRes: StreamResolution =      pendingCapRes === '4mp' ? 'pipe2' : pendingCapRes;
    setCapResDialogOpen(false);
    try {
      setStreamSaving(true);
      const next: StreamConfigReq = { ...streamCfg, resolution: newStreamRes };
      await setStreamConfigReq(next);
      setStreamCfg(next);
      appliedResolutionRef.current = newStreamRes;
      /* Same remount rationale as handleStreamSave: the encoder session is
       * rebuilt and WS clients get a 4001 stream-reset on the fresh SPS. */
      setPlayerEpoch(epoch => epoch + 1);
      toast.success(i18n._('sys.device_tool.stream_saved'));
    } catch (error) {
      console.error('handleCapResConfirm', error);
    } finally {
      setStreamSaving(false);
      setPendingCapRes(null);
    }
  };
  // Trigger method

  const acceptFileType = {
    // Only accept .bin firmware files
    'application/octet-stream': ['.bin'],
  };
  const [isLoading, setIsLoading] = useState(true);
  const getAiStatus = async () => {
    try {
      const res = await getAiStatusReq();
      setIsAiInference(res.data.ai_enabled);
      setAiModelName(res.data.model.name);
      setAiStatus(res.data.model.status);
    } catch (error) {
      console.error('getAiStatus', error);
      throw error;
    }
  };
  const getAiParams = async () => {
    try {
      const res = await getAiParamsReq();
      setAiParams(res.data);
    } catch (error) {
      console.error('getAiParams', error);
      throw error;
    }
  };
  // const initTriggerConfig = async () => {
  //   try {
  //     const res = await getTriggerConfigReq();
  //     setTriggerConfig(res.data);
  //   } catch (error) {
  //     console.error('initTriggerConfig', error);
  //     throw error;
  //   }
  // };
  const initPowerMode = async () => {
    try {
      const res = await getPowerModeReq();
      setCurPowerModel(res.data.current_mode);
    } catch (error) {
      console.error('initPowerMode', error);
      throw error;
    }
  };
  const initSysClk = async () => {
    try {
      const clkRes = await getSysClkConfigReq();
      const d = clkRes.data as { valid?: boolean; sys_clk_profile?: number };
      const valid = Boolean(d?.valid);
      setSysClkFlashValid(valid);
      if (valid && typeof d.sys_clk_profile === 'number') {
        const p = d.sys_clk_profile;
        if (p === 1 || p === 2 || p === 3 || p === 4) {
          setSysClkProfile(p);
        } else {
          setSysClkProfile(4);
        }
      } else {
        setSysClkProfile(4);
      }
    } catch (error) {
      console.error('initSysClk', error);
      setSysClkFlashValid(false);
      setSysClkProfile(4);
    }
  };
  const initQueue = async () => {
    try {
      setIsLoading(true);
      // Initialize AI state to true
      await toggleAiReq({ ai_enabled: true });

      await Promise.allSettled([
        getAiStatus(),
        loadStreamConfig(),
        loadRuntimeCapture(),
        // initTriggerConfig(),
        initPowerMode(),
        getAiParams(),
        initSysClk(),
      ])
    } catch (error) {
      console.error('initQueue', error);
      throw error;
    } finally {
      setIsLoading(false);
    }
  }
  useEffect(() => {
    initQueue();
  }, []);

  const handleAiParamsChange = async (key: string, value: number) => {
    if (value < 1) {
      value = 1;
    } else if (value > 100) {
      value = 100;
    }
    if (Number.isNaN(value)) {
      value = 50;
    }
    setAiParams(p => ({ ...p, [key]: value }));
  };
  const postAiParams = async (key: keyof AiParams, value: number) => {
    const params = { ...aiParams, [key]: value };
    try {
      await setAiParamsReq(params);
    } catch (error) {
      console.error('postAiParams', error);
      throw error;
    }
  };
  const handleUploadChange = async (file: File) => {
    try {
      setModelUploadLoading(true);
      await stopVideoStreamReq();
      const contentPreview = await sliceFile(file, 2048);
      if (!contentPreview.size) {
        throw new Error(i18n._('sys.system_management.invalid_firmware_file') || 'Invalid firmware file');
      }
      await preCheckReq(contentPreview, 'ai');
      await uploadOTAFileReq(file, 'ai');
      await reloadModelReq();
    } catch (error) {
      console.error('handleUploadChange', error);
      throw error;
    } finally {
      try {
        await stopVideoStreamReq();
        await new Promise((resolve) => { setTimeout(resolve, 1000); });
        // Remount player for a clean MSE / Worker / WebSocket lifecycle
        setPlayerEpoch((epoch) => epoch + 1);
        await getAiStatus();
      } catch (restartError) {
        console.error('preview restart after model upload', restartError);
      }
      setModelUploadLoading(false);
    }
  };

  // const handleUploadChange = async (file: File) => {
  const uploadSlot = (
    <>
      <SvgIcon icon="upload" />
      {i18n._('common.upload')}
    </>
  );

  // Feature guide
  const [showToolGuide, setShowToolGuide] = useState(
    getItem('guideFlag') !== 'false'
  );

  const handleToolGuideClose = () => {
    setShowToolGuide(false);
  };

  // Feature guide refs
  // const aiInferenceRef = useRef<HTMLDivElement>(null)
  const currentModelRef = useRef<HTMLDivElement>(null);
  const patternRef = useRef<HTMLDivElement>(null);
  const triggerRef = useRef<HTMLDivElement>(null);
  const scollRef = useRef<HTMLDivElement>(null);
  const refList = [currentModelRef, patternRef, triggerRef];
  return (
    <div ref={scollRef} className="h-full w-full overflow-y-auto">
      {modelUploadLoading && <WifiReloadMask isLoading={modelUploadLoading} loadingText={i18n._('sys.device_tool.upload_model_loading')} />}
      <div className="w-full py-4 px-4 md:px-0  flex justify-center">
        {showToolGuide && !isLoading && (
          <ToolGuide
            refList={refList}
            scollRef={scollRef}
            isLoading={isLoading}
            onClose={handleToolGuideClose}
          />
        )}
        <Card className="relative min-w-0">
          <CardContent className="sm:w-xl flex flex-col">

            <div className=" bg-gray-100 w-full  aspect-video flex justify-center items-center">
              <Player
                key={playerEpoch}
                videoUrl={getWebSocketUrl()}
                videoRendererInstance={videoRendererInstance}
              />
            </div>
            <div
              className=" w-full bg-white pt-4"
            >
              {isLoading ? (
                <DeviceToolSkeleton />
              )
                : (
                  <div>
                    {/* Camera settings */}
                    <p className="text-sm text-text-primary font-semibold mb-2 mt-4">
                      {i18n._('sys.device_tool.camera_title')}
                    </p>
                    <Card className="bg-gray-50 mb-4">
                      <CardContent className="">
                        <div className="w-full h-full ">
                          <div
                            ref={currentModelRef}
                            className="flex items-center justify-between flex-wrap gap-2"
                          >
                            <Label className="text-sm">
                              {i18n._('sys.device_tool.model')}
                            </Label>
                            <div className="flex items-center gap-2 flex-wrap">
                              <p className="text-sm text-text-secondary mr-1">
                                {aiStatus === 'loaded' ? aiModelName : i18n._('sys.model_verification.please_upload_model')}
                              </p>
                              <Upload
                                slot={uploadSlot}
                                className="flex flex-1 h-full justify-start"
                                type="button"
                                accept={acceptFileType}
                                maxFiles={1}
                                maxSize={1024 * 1024 * 10}
                                minSize={0}
                                multiple={false}
                                onFileChange={handleUploadChange}
                              />
                            </div>
                          </div>
                          {aiStatus === 'loaded' && (
                            <>
                              <Separator className="my-2" />
                              <div className="flex items-center justify-between">
                                <p className="text-sm font-semibold text-text-primary">
                                  {i18n._('sys.device_tool.params')}
                                </p>
                              </div>
                              <div className="flex flex-col items-center gap-4 p-2 border border-gray-200 border-solid rounded-md mt-2">
                                <div className="w-full flex items-center justify-between gap-4">
                                  <Label>{i18n._('sys.device_tool.nms_threshold')}</Label>
                                  <div className="flex items-center gap-2 flex-1">
                                    <Slider
                                      className="flex-1 min-w-16"
                                      value={aiParams.nms_threshold}
                                      max={100}
                                      step={1}
                                      min={1}
                                      onChange={value => setAiParams(p => ({ ...p, nms_threshold: value }))}
                                      onChangeEnd={(value) => postAiParams('nms_threshold', value)}
                                    />
                                    <Input
                                      className="w-16"
                                      type="number"
                                      min={1}
                                      max={100}
                                      step={1}
                                      value={aiParams.nms_threshold}
                                      onChange={(e) => handleAiParamsChange('nms_threshold', parseInt((e.target as HTMLInputElement).value || '0', 10))}
                                      onBlur={(e) => postAiParams('nms_threshold', parseInt((e.target as HTMLInputElement).value || '0', 10))}
                                    />
                                  </div>
                                </div>
                                <div className="w-full flex items-center justify-between gap-4">
                                  <Label>{i18n._('sys.device_tool.confidence_threshold')}</Label>
                                  <div className="flex items-center gap-2 flex-1">
                                    <Slider
                                      className="flex-1 min-w-16"
                                      value={aiParams.confidence_threshold}
                                      max={100}
                                      step={1}
                                      min={1}
                                      onChange={value => setAiParams(p => ({ ...p, confidence_threshold: value }))}
                                      onChangeEnd={value => postAiParams('confidence_threshold', value)}
                                    />
                                    <Input
                                      className="w-16"
                                      type="number"
                                      min={1}
                                      max={100}
                                      step={1}
                                      value={aiParams.confidence_threshold}
                                      onChange={(e) => handleAiParamsChange('confidence_threshold', parseInt((e.target as HTMLInputElement).value || '1', 10))}
                                      onBlur={(e) => postAiParams('confidence_threshold', parseInt((e.target as HTMLInputElement).value || '1', 10))}
                                    />
                                  </div>
                                </div>
                              </div>
                            </>
                          )}
                          <Separator className="my-2" />
                          {/* Live video stream parameters — heading row plus a
                           * bordered sub-card, mirroring 媒体流设置 below */}
                          <div className="flex items-center justify-between">
                            <p className="text-sm font-semibold text-text-primary">
                              {i18n._('sys.device_tool.stream_params')}
                            </p>
                          </div>
                          <div className="border border-gray-200 border-solid p-4 rounded-md mt-2 flex flex-col">
                            <div className="flex justify-between gap-2 flex-1 pr-0">
                              <div className="flex items-center gap-2">
                                <Label className="text-sm text-text-primary shrink-0">
                                  {i18n._('sys.device_tool.stream_resolution')}
                                </Label>
                                <Tooltip mbEnhance>
                                  <TooltipTrigger>
                                    <div className="w-4 flex justify-center items-center">
                                      <SvgIcon className="w-4 h-4" icon="info" />
                                    </div>
                                  </TooltipTrigger>
                                  <TooltipContent className="max-w-80 text-pretty">
                                    <p>{i18n._('sys.device_tool.stream_resolution_note')}</p>
                                  </TooltipContent>
                                </Tooltip>
                              </div>
                              <Select
                                value={streamCfg.resolution}
                                onValueChange={value => setStreamCfg(p => ({
                                  ...p,
                                  resolution: value as StreamResolution,
                                }))}
                              >
                                <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
                                  <SelectValue placeholder={i18n._('sys.device_tool.stream_resolution')} />
                                </SelectTrigger>
                                <SelectContent>
                                  <SelectItem value="pipe2">
                                    {i18n._('sys.device_tool.stream_res_pipe2')}
                                  </SelectItem>
                                  <SelectItem value="720p">
                                    {i18n._('sys.device_tool.stream_res_main_720')}
                                  </SelectItem>
                                  <SelectItem value="1080p">
                                    {i18n._('sys.device_tool.stream_res_main_1080')}
                                  </SelectItem>
                                </SelectContent>
                              </Select>
                            </div>
                            <Separator className="my-2" />
                            <div className="flex justify-between gap-2 flex-1 pr-0">
                              <Label className="text-sm text-text-primary shrink-0">
                                {i18n._('sys.device_tool.stream_bitrate')}
                              </Label>
                              <div className="flex items-center gap-1">
                                <NumberField
                                  className="w-24 text-right"
                                  min={256}
                                  max={8192}
                                  value={streamCfg.bitrate_kbps}
                                  onCommit={(v: number) => setStreamCfg(p => ({
                                    ...p,
                                    bitrate_kbps: v,
                                  }))}
                                />
                                <span className="text-xs text-text-secondary">kbps</span>
                              </div>
                            </div>
                            <Separator className="my-2" />
                            <div className="flex justify-between gap-2 flex-1 pr-0">
                              <Label className="text-sm text-text-primary shrink-0">
                                {i18n._('sys.device_tool.stream_gop')}
                              </Label>
                              <Select
                                value={String(streamCfg.gop_sec)}
                                onValueChange={value => setStreamCfg(p => ({
                                  ...p,
                                  gop_sec: Number(value) as 1 | 2 | 4,
                                }))}
                              >
                                <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
                                  <SelectValue placeholder={i18n._('sys.device_tool.stream_gop')} />
                                </SelectTrigger>
                                <SelectContent>
                                  <SelectItem value="1">{i18n._('sys.device_tool.stream_gop_1s')}</SelectItem>
                                  <SelectItem value="2">{i18n._('sys.device_tool.stream_gop_2s')}</SelectItem>
                                  <SelectItem value="4">{i18n._('sys.device_tool.stream_gop_4s')}</SelectItem>
                                </SelectContent>
                              </Select>
                            </div>
                            <div className="flex justify-end mt-4">
                              <Button
                                variant="primary"
                                disabled={streamSaving}
                                onClick={handleStreamSave}
                              >
                                {streamSaving ? (
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
                          </div>
                          <Separator className="my-2" />
                          {/* Runtime capture parameters — same heading+bordered
                           * sub-card style as the stream params block above */}
                          <div className="flex items-center justify-between">
                            <div className="flex items-center gap-2">
                              <p className="text-sm font-semibold text-text-primary">
                                {i18n._('sys.device_tool.runtime_capture_params')}
                              </p>
                              <Tooltip mbEnhance>
                                <TooltipTrigger>
                                  <div className="w-4 flex justify-center items-center">
                                    <SvgIcon className="w-4 h-4" icon="info" />
                                  </div>
                                </TooltipTrigger>
                                <TooltipContent className="max-w-80 text-pretty">
                                  <p>{i18n._('sys.device_tool.runtime_capture_params_note')}</p>
                                </TooltipContent>
                              </Tooltip>
                            </div>
                            {/* Low-power mode: captures while asleep come from
                             * the sleep-wake quick path on the capture-settings
                             * page — jump straight to that section. */}
                            {curPowerModel !== 'full_speed' && (
                              <Button
                                variant="outline"
                                className="h-7 px-2 text-xs"
                                onClick={() => navigate('/capture-settings?focus=sleep-wake')}
                              >
                                {i18n._('sys.device_tool.sleep_capture_shortcut')}
                              </Button>
                            )}
                          </div>
                          <div className="border border-gray-200 border-solid p-4 rounded-md mt-2 flex flex-col">
                            <div className="flex justify-between gap-2 flex-1 pr-0">
                              <div className="flex items-center gap-2">
                                <Label className="text-sm text-text-primary shrink-0">
                                  {i18n._('sys.device_tool.runtime_capture_resolution')}
                                </Label>
                                <Tooltip mbEnhance>
                                  <TooltipTrigger>
                                    <div className="w-4 flex justify-center items-center">
                                      <SvgIcon className="w-4 h-4" icon="info" />
                                    </div>
                                  </TooltipTrigger>
                                  <TooltipContent className="max-w-80 text-pretty">
                                    <p>{i18n._('sys.device_tool.runtime_capture_resolution_hint')}</p>
                                  </TooltipContent>
                                </Tooltip>
                              </div>
                              <Select
                                value={captureResFromStream}
                                onValueChange={handleCapResChange}
                              >
                                <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
                                  <SelectValue placeholder={i18n._('sys.device_tool.runtime_capture_resolution')} />
                                </SelectTrigger>
                                <SelectContent>
                                  <SelectItem value="4mp">4MP</SelectItem>
                                  <SelectItem value="720p">720P</SelectItem>
                                  <SelectItem value="1080p">1080P</SelectItem>
                                </SelectContent>
                              </Select>
                            </div>
                            <Separator className="my-2" />
                            <div className="flex justify-between gap-2 flex-1 pr-0">
                              <Label className="text-sm text-text-primary shrink-0">
                                {i18n._('sys.device_tool.runtime_capture_skip_frames')}
                              </Label>
                              <NumberField
                                className="w-24 text-right"
                                min={0}
                                max={30}
                                value={runtimeCap.runtime_capture_skip_frames}
                                onCommit={(v: number) => setRuntimeCap(p => ({
                                  ...p,
                                  runtime_capture_skip_frames: v,
                                }))}
                              />
                            </div>
                            <Separator className="my-2" />
                            <div className="flex justify-between gap-2 flex-1 pr-0">
                              <Label className="text-sm text-text-primary shrink-0">
                                {i18n._('sys.device_tool.runtime_capture_jpeg_quality')}
                              </Label>
                              <NumberField
                                className="w-24 text-right"
                                min={1}
                                max={100}
                                value={runtimeCap.runtime_capture_jpeg_quality}
                                onCommit={(v: number) => setRuntimeCap(p => ({
                                  ...p,
                                  runtime_capture_jpeg_quality: v,
                                }))}
                              />
                            </div>
                            <div className="flex justify-end mt-4">
                              <Button
                                variant="primary"
                                disabled={runtimeCapSaving}
                                onClick={handleRuntimeCapSave}
                              >
                                {runtimeCapSaving ? (
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
                          </div>
                          {/* Confirm dialog: changing the capture resolution
                           * switches the stream resolution with it */}
                          <Dialog open={capResDialogOpen} onOpenChange={setCapResDialogOpen}>
                            <DialogContent className="md:max-w-md mx-4">
                              <DialogHeader>
                                <DialogTitle>
                                  {i18n._('sys.device_tool.runtime_capture_res_confirm_title')}
                                </DialogTitle>
                                <DialogDescription className="pt-2 text-left flex flex-col gap-1">
                                  {i18n._('sys.device_tool.runtime_capture_res_confirm_body')
                                    .replace('{resolution}', pendingCapRes ? capResDialogLabel(pendingCapRes) : '')}
                                </DialogDescription>
                              </DialogHeader>
                              <DialogFooter className="mt-4">
                                <Button
                                  variant="outline"
                                  onClick={() => {
                                    setCapResDialogOpen(false);
                                    setPendingCapRes(null);
                                  }}
                                >
                                  {i18n._('common.cancel')}
                                </Button>
                                <Button variant="primary" onClick={handleCapResConfirm}>
                                  {i18n._('common.confirm')}
                                </Button>
                              </DialogFooter>
                            </DialogContent>
                          </Dialog>
                          <Separator className="my-2" />
                          <div ref={patternRef}>
                            <div className="flex items-center  justify-between">
                              <Label className="text-sm text-text-primary">
                                {i18n._('sys.device_tool.power')}
                              </Label>
                              <Select
                                value={curPowerModel}
                                onValueChange={handlePowerModelChange}
                              >
                                <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
                                  <SelectValue
                                    placeholder={i18n._('sys.device_tool.power')}
                                  />
                                </SelectTrigger>
                                <SelectContent>
                                  <SelectItem value="full_speed">
                                    {i18n._('sys.device_tool.full_speed')}
                                  </SelectItem>
                                  <SelectItem value="low_power">
                                    {i18n._('sys.device_tool.low_power')}
                                  </SelectItem>
                                </SelectContent>
                              </Select>
                            </div>
                            <Separator className="my-2" />
                            <div className="flex items-center justify-between">
                              <Label className="inline-flex min-w-0 shrink-0 items-center gap-1 text-sm text-text-primary">
                                {i18n._('sys.device_tool.sys_clk_title')}
                                  <Tooltip mbEnhance>
                                    <TooltipTrigger asChild>
                                      <button
                                        type="button"
                                        className="inline-flex size-4 shrink-0 items-center justify-center text-gray-500"
                                        aria-label={i18n._('common.tip')}
                                      >
                                        <SvgIcon className="size-4 text-gray-500" icon="info" />
                                      </button>
                                    </TooltipTrigger>
                                    <TooltipContent className="max-w-80 text-pretty">
                                      <div>
                                        <p>{i18n._('sys.device_tool.sys_clk_reboot_hint')}</p>
                                        {!sysClkFlashValid && (
                                          <p className="mt-2">{i18n._('sys.device_tool.sys_clk_no_valid_profile')}</p>
                                        )}
                                      </div>
                                    </TooltipContent>
                                  </Tooltip>
                              </Label>
                                <Select
                                  value={String(sysClkProfile)}
                                  onValueChange={async (v) => {
                                    const next = Number(v) as 1 | 2 | 3 | 4;
                                    setSysClkProfile(next);
                                    try {
                                      await setSysClkConfigReq({ sys_clk_profile: next });
                                      setSysClkFlashValid(true);
                                      toast.warning(i18n._('sys.device_tool.sys_clk_reboot_toast'));
                                    } catch (e) {
                                      console.error(e);
                                      toast.error('Save failed');
                                    }
                                  }}
                                >
                                  <SelectTrigger className="border-0 shadow-none focus-visible:ring-0 focus-visible:border-transparent">
                                    <SelectValue placeholder={i18n._('sys.device_tool.sys_clk_title')} />
                                  </SelectTrigger>
                                  <SelectContent>
                                    <SelectItem value="1">{i18n._('sys.device_tool.sys_clk_hse_200')}</SelectItem>
                                    <SelectItem value="2">{i18n._('sys.device_tool.sys_clk_hse_400')}</SelectItem>
                                    <SelectItem value="3">{i18n._('sys.device_tool.sys_clk_hsi_800')}</SelectItem>
                                    <SelectItem value="4">{i18n._('sys.device_tool.sys_clk_hse_800')}</SelectItem>
                                  </SelectContent>
                                </Select>
                            </div>
                            <Separator className="my-2" />
                            <RtmpConfig />
                          </div>
                        </div>
                      </CardContent>
                    </Card>
                    <TriggerConfig
                      childeRef={triggerRef}
                    />
                  </div>
                )}
            </div>
          </CardContent>
        </Card>
      </div>
    </div>
  );
}
