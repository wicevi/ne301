/**
 * Preview-stream lifecycle manager.
 *
 * Pages mount → start, unmount → stop. With fast page switches (preview ↔
 * face recognition) the OLD page's fire-and-forget stop can reach the device
 * AFTER the new page's start, killing the fresh stream → black player.
 *
 * Rules:
 *  - All start/stop requests funnel through ONE serial queue, so the device
 *    sees them in exactly the order they were issued.
 *  - Page-unmount stops are DEBOUNCED (STOP_DEBOUNCE_MS): when the next page
 *    starts within the window the stop is cancelled entirely (the server's
 *    start is idempotent anyway - "already_active").
 *  - previewStreamStopNow() bypasses the debounce for flows that MUST stop
 *    (model upload: the stream stays down for seconds, not milliseconds).
 */
import deviceTool from '@/services/api/deviceTool';

const STOP_DEBOUNCE_MS = 400;

let chain: Promise<unknown> = Promise.resolve();
let pendingStopTimer: ReturnType<typeof setTimeout> | null = null;

function enqueue(task: () => Promise<unknown>): Promise<unknown> {
  chain = chain.then(task, task);
  return chain;
}

/** Start (or keep) the preview stream. Cancels a pending debounced stop. */
export function previewStreamStart(): Promise<unknown> {
  if (pendingStopTimer) {
    clearTimeout(pendingStopTimer);
    pendingStopTimer = null;
  }
  return enqueue(() => deviceTool.startVideoStreamReq().catch((e) => {
    console.error('previewStreamStart', e);
  }));
}

/** Stop on page unmount - debounced, cancelled by the next page's start. */
export function previewStreamStop(): void {
  if (pendingStopTimer) {
    clearTimeout(pendingStopTimer);
  }
  pendingStopTimer = setTimeout(() => {
    pendingStopTimer = null;
    enqueue(() => deviceTool.stopVideoStreamReq().catch((e) => {
      console.error('previewStreamStop', e);
    }));
  }, STOP_DEBOUNCE_MS);
}

/** Stop immediately (model upload path): debounced stop is flushed/cancelled
 *  and the real stop is queued right away. */
export function previewStreamStopNow(): Promise<unknown> {
  if (pendingStopTimer) {
    clearTimeout(pendingStopTimer);
    pendingStopTimer = null;
  }
  return enqueue(() => deviceTool.stopVideoStreamReq().catch((e) => {
    console.error('previewStreamStopNow', e);
  }));
}
