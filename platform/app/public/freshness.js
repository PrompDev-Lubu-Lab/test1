export const STALE_AFTER_MS = 45000;

/** Server-observed age advances locally even when the next API request fails. */
export function heartbeatFreshness(instance, nowMs, synthetic = false) {
  if (synthetic) return { kind: 'synthetic', label: 'Frozen synthetic heartbeat', age: null };
  const initial = instance?.heartbeat?.age_seconds, received = instance?._receivedAtMs;
  if (!Number.isFinite(initial) || initial < 0 || !Number.isFinite(received) || !Number.isFinite(nowMs)) return { kind:'unknown', label:'Heartbeat freshness unavailable', age:null };
  const age = initial + Math.max(0, nowMs - received) / 1000;
  const kind = age * 1000 >= STALE_AFTER_MS ? 'stale' : 'recent';
  return {kind,age,label:`${kind === 'stale' ? 'Stale' : 'Recent'} heartbeat · ${Math.floor(age)}s ago`};
}
