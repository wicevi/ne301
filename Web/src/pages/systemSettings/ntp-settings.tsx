import { useState, useEffect, useCallback } from 'preact/hooks';
import { useLingui } from '@lingui/react';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { Input } from '@/components/ui/input';
import { Button } from '@/components/ui/button';
import { Skeleton } from '@/components/ui/skeleton';
import { Separator } from '@/components/ui/separator';
import { toast } from 'sonner';
import SvgIcon from '@/components/svg-icon';
import systemApis, { type NtpConfig } from '@/services/api/system';
import { isValidPoeIp, isValidHostname } from '@/utils/verify';

const NTP_SERVER_MAX_COUNT = 4;
const NTP_SERVER_NAME_MAX = 63;

/** An NTP server may be a hostname or a plain IPv4 address. */
const isValidNtpServer = (value: string) => {
    const v = value.trim();
    return v.length <= NTP_SERVER_NAME_MAX && (isValidHostname(v) || isValidPoeIp(v));
};

/** NTP section embedded in the "Device Configuration" page, placed ABOVE the
 *  USB section. The enable switch applies instantly (POST {enable}); the
 *  server list is edited locally and applied with the save button. */
export default function NtpSettings() {
    const { i18n } = useLingui();
    const { getNtpConfigReq, setNtpConfigReq } = systemApis;
    const [loading, setLoading] = useState(true);
    const [loadFailed, setLoadFailed] = useState(false);
    const [enable, setEnable] = useState(true);
    const [running, setRunning] = useState<boolean | undefined>(undefined);
    const [servers, setServers] = useState<string[]>(['']);
    const [serverErrors, setServerErrors] = useState<boolean[]>([false]);
    const [switching, setSwitching] = useState(false);
    const [saveLoading, setSaveLoading] = useState(false);

    const loadConfig = useCallback(async () => {
        try {
            setLoading(true);
            setLoadFailed(false);
            const res = await getNtpConfigReq();
            const cfg: NtpConfig = res.data;
            setEnable(cfg.enable);
            setRunning(cfg.running);
            setServers(cfg.servers && cfg.servers.length > 0 ? [...cfg.servers] : ['']);
            setServerErrors(new Array(cfg.servers?.length || 1).fill(false));
        } catch (error) {
            console.error('getNtpConfig', error);
            setLoadFailed(true);
        } finally {
            setLoading(false);
        }
    }, []);

    useEffect(() => {
        loadConfig();
    }, [loadConfig]);

    /** Enable switch: instant apply, no save button (same UX as the USB switch). */
    const handleToggle = async (checked: boolean) => {
        if (switching) return;
        const prev = enable;
        setEnable(checked); // optimistic
        setSwitching(true);
        try {
            const res = await setNtpConfigReq({ enable: checked });
            if (res.data) setRunning(res.data.running);
        } catch (error) {
            console.error('setNtpConfig enable', error);
            setEnable(prev);
        } finally {
            setSwitching(false);
        }
    };

    const patchServer = (index: number, value: string) => {
        setServers(prev => prev.map((s, i) => (i === index ? value : s)));
        setServerErrors(prev => prev.map((e, i) => (i === index ? false : e)));
    };

    const addServer = () => {
        if (servers.length >= NTP_SERVER_MAX_COUNT) return;
        setServers(prev => [...prev, '']);
        setServerErrors(prev => [...prev, false]);
    };

    const removeServer = (index: number) => {
        setServers(prev => prev.filter((_, i) => i !== index));
        setServerErrors(prev => prev.filter((_, i) => i !== index));
    };

    const handleSave = async () => {
        const trimmed = servers.map(s => s.trim());
        const errs = trimmed.map(s => s !== '' && !isValidNtpServer(s));
        setServerErrors(errs);
        const cleaned = trimmed.filter(s => s !== '');
        if (errs.some(e => e) || (enable && cleaned.length === 0)) {
            toast.error(enable && cleaned.length === 0
                ? i18n._('sys.system_management.ntp_need_server')
                : i18n._('sys.system_management.ntp_invalid_server'));
            return;
        }
        try {
            setSaveLoading(true);
            const res = await setNtpConfigReq({ enable, servers: cleaned });
            if (res.data) {
                setRunning(res.data.running);
                if (Array.isArray(res.data.servers)) {
                    setServers(res.data.servers.length > 0 ? [...res.data.servers] : ['']);
                    setServerErrors(new Array(res.data.servers.length || 1).fill(false));
                }
            }
            toast.success(i18n._('sys.system_management.ntp_save_ok'));
        } catch (error) {
            console.error('setNtpConfig servers', error);
        } finally {
            setSaveLoading(false);
        }
    };

    if (loading) {
        return (
            <>
                <p className="text-sm text-text-primary font-bold mt-4 mb-2">
                    {i18n._('sys.system_management.ntp_title')}
                </p>
                <div className="bg-gray-100 p-4 rounded-lg">
                    <Skeleton className="h-10 w-full" />
                </div>
            </>
        );
    }

    if (loadFailed) {
        return (
            <>
                <p className="text-sm text-text-primary font-bold mt-4 mb-2">
                    {i18n._('sys.system_management.ntp_title')}
                </p>
                <div className="bg-gray-100 p-4 rounded-lg">
                    <p className="text-sm text-gray-400">{i18n._('sys.system_management.ntp_load_failed')}</p>
                </div>
            </>
        );
    }

    return (
        <>
            <p className="text-sm text-text-primary font-bold mt-4 mb-2">
                {i18n._('sys.system_management.ntp_title')}
            </p>
            <div className="flex flex-col gap-2 bg-gray-100 p-4 rounded-lg">
                {/* Enable switch: plain label-left / switch-right row like the
                    login-password section; hint and status are full-width
                    lines below the row. */}
                <div className="flex justify-between items-center">
                    <Label className="text-sm text-text-primary shrink-0">{i18n._('sys.system_management.ntp_enable')}</Label>
                    <Switch
                      checked={enable}
                      onCheckedChange={handleToggle}
                      disabled={switching}
                    />
                </div>
                <p className="text-xs text-gray-400">{i18n._('sys.system_management.ntp_switch_hint')}</p>
                <p className={`text-xs font-medium ${running ? 'text-green-600' : 'text-gray-500'}`}>
                    {running === undefined
                        ? '-'
                        : i18n._(running
                            ? 'sys.system_management.ntp_status_running'
                            : 'sys.system_management.ntp_status_stopped')}
                </p>

                {/* Server list, relevant while enabled */}
                {enable && (
                    <>
                        <Separator />
                        <div className="flex justify-between items-center">
                            <Label className="text-sm text-text-primary shrink-0">{i18n._('sys.system_management.ntp_servers')}</Label>
                            {servers.length < NTP_SERVER_MAX_COUNT && (
                                <Button variant="outline" size="sm" onClick={addServer} disabled={saveLoading}>
                                    <SvgIcon icon="add" className="w-4 h-4 mr-1" />
                                    {i18n._('sys.system_management.ntp_add')}
                                </Button>
                            )}
                        </div>
                        {servers.map((server, index) => (
                            <div key={index} className="flex flex-col gap-1">
                                <Separator />
                                <div className="flex justify-between gap-2 items-center">
                                    <Input
                                      variant="ghost"
                                      value={server}
                                      maxLength={NTP_SERVER_NAME_MAX}
                                      placeholder="ntp.aliyun.com"
                                      className="text-sm text-text-primary text-right"
                                      onChange={(e) => patchServer(index, (e.target as HTMLInputElement).value)}
                                    />
                                    {servers.length > 1 && (
                                        <Button variant="ghost" size="sm" onClick={() => removeServer(index)} disabled={saveLoading}>
                                            <SvgIcon icon="close" className="w-4 h-4" />
                                        </Button>
                                    )}
                                </div>
                                {serverErrors[index] && (
                                    <p className="text-red-500 text-sm self-end">
                                        {i18n._('sys.system_management.format_error')}
                                    </p>
                                )}
                            </div>
                        ))}
                        <p className="text-xs text-gray-400">{i18n._('sys.system_management.ntp_server_hint')}</p>
                    </>
                )}
            </div>
            {/* Save lives OUTSIDE the card, bottom-right — same as the
                login-password section above. */}
            {enable && (
                <Button variant="primary" className="mt-4 grow-0 self-end" onClick={handleSave} disabled={saveLoading}>
                    {saveLoading && <div className="w-4 h-4 mr-2 animate-spin rounded-full border-2 border-white border-t-transparent" />}
                    {i18n._('common.save')}
                </Button>
            )}
        </>
    );
}
