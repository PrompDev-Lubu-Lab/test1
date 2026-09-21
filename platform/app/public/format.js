// Presentation helpers do not rename or mutate API records.
export function formatFraction(value) {
  const validString = typeof value === 'string' && /^-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?$/.test(value);
  if (typeof value !== 'number' && !validString) return '—';
  const number = Number(value);
  return Number.isFinite(number) ? `${(number * 100).toFixed(2)}%` : '—';
}
export function runDrawdownFraction(run) {
  // A summary's max_drawdown is money; the index's identically named field is a fraction.
  return Object.hasOwn(run,'max_drawdown_fraction') ? run.max_drawdown_fraction : run.max_drawdown;
}
export function runLabel(run) {
  if (typeof run.label === 'string' && run.label) return run.label;
  const labels = Array.isArray(run.strategies) ? run.strategies.map(item => item.label).filter(label => typeof label === 'string' && label) : [];
  return labels.join(', ') || run.run_id || 'Unlabelled run';
}
