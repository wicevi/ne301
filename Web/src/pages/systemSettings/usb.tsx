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

    /* After a USB change that can reset the connection carrying the request
     * (lwIP drops TCP connections bound to the netif's old address - and in
     * follow-AP mode the RNDIS netif holds a duplicate of the hotspot
     * address, so ANY usb change can RST the client's own link, especially
     * on dual-homed PCs wired via USB AND on the hotspot WiFi), poll the
     * device's ACTUAL state with short deadlines and report that. */
    const waitMs = (ms: number) => new Promise<void>((resolve) => {
            setTimeout(resolve, ms);
        });

    /* One bounded (4s AbortController) status attempt, null on failure. */
    const attemptStatusFetch = async (): Promise<UsbStatus | null> => {
        const ctl = new AbortController();
        const timer = setTimeout(() => ctl.abort(), 4000);
        try {
            const res = await getUsbStatusReq({ skipErrorToast: true, signal: ctl.signal });
            const d = res.data ?? {};
            return {
                enabled: !!d.enabled,
                running: !!d.running,
                rndis_active: !!d.rndis_active,
                via_usb: !!d.via_usb,
                follow_ap: !!d.follow_ap,
                ip: String(d.ip ?? ''),
                netmask: String(d.netmask ?? ''),
            };
        } catch {
            return null;
        } finally {
            clearTimeout(timer);
        }
    };

    /* Sequential retries while the connection settles after the change. */
    const fetchStatusQuick = async (): Promise<UsbStatus | null> => {
        await waitMs(800);
        const first = await attemptStatusFetch();
        if (first) return first;
        await waitMs(1500);
        const second = await attemptStatusFetch();
        if (second) return second;
        await waitMs(1500);
        return attemptStatusFetch();
    };

    const applyToggle = async (checked: boolean) => {
        if (!status || switching) return;
        const prev = status;
        // optimistic: flip immediately, restore on failure
        setStatus({ ...status, enabled: checked });
        setSwitching(true);
        const viaUsb = status.via_usb;

        /* Outcomes are handled here: a disable can reset the request's own
         * connection (see fetchStatusQuick) - we verify the device state
         * instead of trusting the network error. */
        const reqCfg = { skipErrorToast: true };

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
                /* Verify what the device actually did (the request may have
                 * been RST'd mid-flight although the change was applied). */
                const st = await fetchStatusQuick();
                if (st && st.enabled === checked) {
                    setStatus(checked ? { ...st, via_usb: st.via_usb } : { ...st, enabled: false, running: false, rndis_active: false, via_usb: false });
                    toast.success(checked
                        ? i18n._('sys.system_management.usb_enable_ok')
                        : i18n._('sys.system_management.usb_disable_ok'));
                } else {
                    setStatus(prev);
                    toast.error(i18n._('sys.system_management.usb_apply_failed'));
                }
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
        /* Always suppress the global error toast: applying an address change
         * can reset the very connection the request rides on (lwIP drops
         * connections bound to the old address - the follow-AP RNDIS netif
         * holds a duplicate of the hotspot address), so a network error here
         * does not mean the change failed. Outcomes are reported below after
         * verifying the device's actual state. */
        const reqCfg = { skipErrorToast: true };
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
            } else {
                /* Connection likely reset by the address change itself.
                 * Re-connect and check what the device actually applied. */
                const st = await fetchStatusQuick();
                const applied = !!st && !!st.follow_ap === followAp
                    && (followAp || (st.ip === ip && st.netmask === netmask));
                if (applied && st) {
                    applyStatus(st);
                    toast.success(i18n._('sys.system_management.usb_apply_ok'));
                } else {
                    toast.error(i18n._('sys.system_management.usb_apply_failed'));
                }
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
                    </div>
                )}
                {/* Apply stays visible in BOTH modes: switching back to
                 * follow-ap is also a saveable change - hiding the button
                 * with the custom fields made that state impossible to
                 * apply (or recover from). */}
                <Button
                  variant="primary"
                  className="self-end"
                  disabled={saving || !addressDirty()}
                  onClick={handleSaveClick}
                >
                    {i18n._('sys.system_management.usb_apply')}
                </Button>
            </div>
            {confirmOffDialog()}
            {confirmIpDialog()}
        </>
    );
}
