import { useState, useEffect, useCallback } from 'preact/hooks';
import { useLingui } from '@lingui/react';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { Skeleton } from '@/components/ui/skeleton';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { Dialog, DialogContent, DialogHeader, DialogTitle, DialogFooter } from '@/components/dialog';
import { toast } from 'sonner';
import systemSettings from '@/services/api/systemSettings';

interface UsbStatus {
    enabled: boolean;
    running: boolean;
    rndis_active: boolean;
    /** true when THIS page's requests arrive over the USB RNDIS link
     *  (client IP == the fixed USB lease; reported by the device). */
    via_usb: boolean;
    follow_ap: boolean;
    ip: string;
    netmask: string;
}

const IPV4_RE = /^(\d{1,3}\.){3}\d{1,3}$/;

const octetsOk = (s: string) => IPV4_RE.test(s) && s.split('.').every((o) => Number(o) <= 255);

/** USB section embedded in the "Device Configuration" page (device-password.tsx):
 *  master switch for the USB device (CDC console + RNDIS NIC) and the RNDIS
 *  netif addressing (follow hotspot = one shared address, or a custom
 *  subnet). Toggling the switch applies immediately — like other instant
 *  switches; address changes save explicitly and bounce the device. */
export default function UsbSettings() {
    const { i18n } = useLingui();
    const { getUsbStatusReq, setUsbConfigReq } = systemSettings;
    const [loading, setLoading] = useState(true);
    const [status, setStatus] = useState<UsbStatus | null>(null);
    const [switching, setSwitching] = useState(false);
    const [isConfirmOffDialogOpen, setIsConfirmOffDialogOpen] = useState(false);
    const [isConfirmIpDialogOpen, setIsConfirmIpDialogOpen] = useState(false);

    /* address form (mirrors the loaded config; user edits, then saves) */
    const [followAp, setFollowAp] = useState(true);
    const [ip, setIp] = useState('');
    const [netmask, setNetmask] = useState('');
    const [ipError, setIpError] = useState('');
    const [saving, setSaving] = useState(false);

    const applyStatus = (s: UsbStatus) => {
        setStatus(s);
        setFollowAp(s.follow_ap);
        setIp(s.ip);
        setNetmask(s.netmask);
        setIpError('');
    };

    const loadStatus = useCallback(async () => {
        try {
            setLoading(true);
            const res = await getUsbStatusReq();
            applyStatus({
                enabled: !!res.data.enabled,
                running: !!res.data.running,
                rndis_active: !!res.data.rndis_active,
                via_usb: !!res.data.via_usb,
                follow_ap: !!res.data.follow_ap,
                ip: String(res.data.ip ?? ''),
                netmask: String(res.data.netmask ?? ''),
            });
        } catch (error) {
            console.error('getUsbStatus', error);
        } finally {
            setLoading(false);
        }
    }, []);

    useEffect(() => {
        loadStatus();
    }, [loadStatus]);

    const applyToggle = async (checked: boolean) => {
        if (!status || switching) return;
        const prev = status;
        // optimistic: flip immediately, restore on failure
        setStatus({ ...status, enabled: checked });
        setSwitching(true);

        // Disabling while THIS page rides the USB RNDIS link: the device
        // tears the NIC down synchronously inside the HTTP handler, so the
        // request may die mid-flight — a network error there IS the success
        // confirmation (skip the global error toast for it).
        const viaUsb = status.via_usb;
        const reqCfg = (!checked && viaUsb) ? { skipErrorToast: true } : undefined;

        try {
            const res = await setUsbConfigReq({
                enabled: checked,
                follow_ap: prev.follow_ap,
                ip_address: prev.ip,
                netmask: prev.netmask,
            }, reqCfg);
            setStatus({
                enabled: !!res.data.enabled,
                running: !!res.data.running,
                rndis_active: prev.rndis_active,
                via_usb: checked ? viaUsb : false,
                follow_ap: !!res.data.follow_ap,
                ip: String(res.data.ip ?? prev.ip),
                netmask: String(res.data.netmask ?? prev.netmask),
            });
            toast.success(checked
                ? i18n._('sys.system_management.usb_enable_ok')
                : i18n._('sys.system_management.usb_disable_ok'));
        } catch (error) {
            console.error('setUsbConfig', error);
            if (!checked && viaUsb) {
                // expected outcome: the NIC that carried this request is gone
                setStatus({ ...prev, enabled: false, running: false, rndis_active: false, via_usb: false });
                toast.success(i18n._('sys.system_management.usb_disable_ok'));
            } else {
                setStatus(prev);
            }
        } finally {
            setSwitching(false);
        }
    };

    const handleToggle = (checked: boolean) => {
        // Disabling while THIS page rides the USB link: confirm first —
        // the connection dies with the NIC and the user must switch to the
        // device hotspot (same address: 192.168.10.10).
        if (!checked && status?.via_usb) {
            setIsConfirmOffDialogOpen(true);
            return;
        }
        applyToggle(checked);
    };

    const addressDirty = () => !!status && (followAp !== status.follow_ap
        || ip !== status.ip || netmask !== status.netmask);

    const saveAddress = async () => {
        if (!status || saving || !addressDirty()) return;
        if (!followAp && (!octetsOk(ip) || !octetsOk(netmask))) {
            setIpError(i18n._('sys.system_management.usb_ip_invalid'));
            return;
        }
        const viaUsb = status.via_usb;
        const reqCfg = viaUsb ? { skipErrorToast: true } : undefined;
        setSaving(true);
        try {
            const res = await setUsbConfigReq({
                enabled: status.enabled,
                follow_ap: followAp,
                ip_address: ip,
                netmask,
            }, reqCfg);
            applyStatus({
                ...status,
                follow_ap: !!res.data.follow_ap,
                ip: String(res.data.ip ?? ip),
                netmask: String(res.data.netmask ?? netmask),
            });
            toast.success(i18n._('sys.system_management.usb_apply_ok'));
        } catch (error) {
            console.error('setUsbConfig(address)', error);
            if (viaUsb) {
                // the USB NIC bounced with the re-enumeration — treat as applied
                toast.success(i18n._('sys.system_management.usb_apply_ok'));
            }
        } finally {
            setSaving(false);
        }
    };

    const handleSaveClick = () => {
        // applying an address change over the USB link re-enumerates the NIC
        if (status?.via_usb && addressDirty()) {
            setIsConfirmIpDialogOpen(true);
            return;
        }
        saveAddress();
    };

    const confirmOffDialog = () => (
        <Dialog open={isConfirmOffDialogOpen} onOpenChange={setIsConfirmOffDialogOpen}>
            <DialogContent className="mx-8">
                <DialogHeader>
                    <DialogTitle>{i18n._('sys.system_management.usb_off_via_rndis_title')}</DialogTitle>
                    <div className="text-sm text-text-primary my-4">{i18n._('sys.system_management.usb_off_via_rndis_body')}</div>
                </DialogHeader>
                <DialogFooter>
                    <Button className="md:w-auto w-1/2" variant="outline" onClick={() => setIsConfirmOffDialogOpen(false)}>{i18n._('common.cancel')}</Button>
                    <Button className="md:w-auto w-1/2" variant="primary" onClick={() => { setIsConfirmOffDialogOpen(false); applyToggle(false); }}>{i18n._('sys.system_management.usb_off_confirm')}</Button>
                </DialogFooter>
            </DialogContent>
        </Dialog>
    );

    const confirmIpDialog = () => (
        <Dialog open={isConfirmIpDialogOpen} onOpenChange={setIsConfirmIpDialogOpen}>
            <DialogContent className="mx-8">
                <DialogHeader>
                    <DialogTitle>{i18n._('sys.system_management.usb_apply_via_usb_title')}</DialogTitle>
                    <div className="text-sm text-text-primary my-4">{i18n._('sys.system_management.usb_apply_via_usb_body')}</div>
                </DialogHeader>
                <DialogFooter>
                    <Button className="md:w-auto w-1/2" variant="outline" onClick={() => setIsConfirmIpDialogOpen(false)}>{i18n._('common.cancel')}</Button>
                    <Button className="md:w-auto w-1/2" variant="primary" onClick={() => { setIsConfirmIpDialogOpen(false); saveAddress(); }}>{i18n._('common.confirm')}</Button>
                </DialogFooter>
            </DialogContent>
        </Dialog>
    );

    if (loading) {
        return (
            <>
                <p className="text-sm text-text-primary font-bold mt-4 mb-2">
                    {i18n._('sys.system_management.usb_title')}
                </p>
                <div className="bg-gray-100 p-4 rounded-lg">
                    <Skeleton className="h-10 w-full" />
                </div>
            </>
        );
    }

    if (!status) {
        return (
            <>
                <p className="text-sm text-text-primary font-bold mt-4 mb-2">
                    {i18n._('sys.system_management.usb_title')}
                </p>
                <div className="bg-gray-100 p-4 rounded-lg">
                    <p className="text-sm text-gray-400">{i18n._('sys.system_management.usb_load_failed')}</p>
                </div>
            </>
        );
    }

    return (
        <>
            <p className="text-sm text-text-primary font-bold mt-4 mb-2">
                {i18n._('sys.system_management.usb_title')}
            </p>
            <div className="flex flex-col gap-2 bg-gray-100 p-4 rounded-lg">
                <div className="flex justify-between items-center">
                    <div className="flex flex-col gap-1 max-w-[70%]">
                        <Label className="text-sm text-text-primary shrink-0">{i18n._('sys.system_management.usb_switch')}</Label>
                        <p className="text-xs text-gray-400">{i18n._('sys.system_management.usb_switch_hint')}</p>
                        <p className={`text-xs font-medium ${status.running ? 'text-green-600' : 'text-gray-500'}`}>
                            {status.running
                                ? i18n._('sys.system_management.usb_running')
                                : i18n._('sys.system_management.usb_stopped')}
                        </p>
                    </div>
                    <Switch
                      checked={status.enabled}
                      onCheckedChange={handleToggle}
                      disabled={switching}
                    />
                </div>

                <div className="h-px bg-border" />

                <div className="flex justify-between items-center">
                    <Label className="text-sm text-text-primary shrink-0">{i18n._('sys.system_management.usb_ip_mode')}</Label>
                    <Select
                      value={followAp ? 'follow' : 'custom'}
                      onValueChange={(v) => setFollowAp(v === 'follow')}
                    >
                        <SelectTrigger
                          className="bg-transparent border-0 !shadow-none !outline-none
             focus:!outline-none focus:!ring-0 focus:!ring-offset-0 focus:!shadow-none focus:!border-transparent
             focus-visible:!outline-none focus-visible:!ring-0 focus-visible:!ring-offset-0
             text-right"
                        >
                            <SelectValue />
                        </SelectTrigger>
                        <SelectContent>
                            <SelectItem value="follow">{i18n._('sys.system_management.usb_ip_follow_ap')}</SelectItem>
                            <SelectItem value="custom">{i18n._('sys.system_management.usb_ip_custom')}</SelectItem>
                        </SelectContent>
                    </Select>
                </div>

                {!followAp && (
                    <div className="flex flex-col gap-2 mt-1">
                        <div className="flex justify-between items-center">
                            <Label className="text-sm text-text-primary shrink-0">{i18n._('sys.system_management.usb_ip_label')}</Label>
                            <Input
                              variant="ghost"
                              value={ip}
                              onChange={(e) => { setIp((e.target as HTMLInputElement).value); setIpError(''); }}
                              placeholder="192.168.20.1"
                              className="text-sm text-text-primary text-right"
                            />
                        </div>
                        <div className="flex justify-between items-center">
                            <Label className="text-sm text-text-primary shrink-0">{i18n._('sys.system_management.usb_netmask_label')}</Label>
                            <Input
                              variant="ghost"
                              value={netmask}
                              onChange={(e) => { setNetmask((e.target as HTMLInputElement).value); setIpError(''); }}
                              placeholder="255.255.255.0"
                              className="text-sm text-text-primary text-right"
                            />
                        </div>
                        <p className="text-xs text-gray-400">{i18n._('sys.system_management.usb_ip_hint')}</p>
                        {ipError && <p className="text-xs text-red-500 self-end">{ipError}</p>}
                        <Button
                          variant="primary"
                          className="self-end"
                          disabled={saving || !addressDirty()}
                          onClick={handleSaveClick}
                        >
                            {i18n._('sys.system_management.usb_apply')}
                        </Button>
                    </div>
                )}
            </div>
            {confirmOffDialog()}
            {confirmIpDialog()}
        </>
    );
}
