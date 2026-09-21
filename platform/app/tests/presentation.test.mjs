import test from 'node:test';
import assert from 'node:assert/strict';
import {formatFraction,runDrawdownFraction,runLabel} from '../public/format.js';
test('summary monetary drawdown is never formatted as an index percentage',() => {
  const summary={run_id:'sample',max_drawdown:'27.68',max_drawdown_fraction:0.0026286752613527854,strategies:[{id:1,label:'ma_synthetic'}],fills:7};
  assert.equal(formatFraction(runDrawdownFraction(summary)),'0.26%');
  assert.equal(summary.max_drawdown,'27.68');
  assert.equal(runLabel(summary),'ma_synthetic');
  assert.equal(summary.round_trips,undefined);
  assert.equal(formatFraction(runDrawdownFraction({...summary,max_drawdown_fraction:null})),'—');
});
test('index fraction strings are presentation-only numbers; absent or invalid values stay unavailable',() => {
  const index={label:'ma_index',max_drawdown:'0.0026',total_return:'0.053001883'};
  assert.equal(formatFraction(runDrawdownFraction(index)),'0.26%');
  assert.equal(formatFraction(index.total_return),'5.30%');
  assert.equal(index.total_return,'0.053001883');
  assert.equal(runLabel(index),'ma_index');
  for(const invalid of ['',null,undefined,'NaN','Infinity',Infinity,'0.05junk',{},true]) assert.equal(formatFraction(invalid),'—');
});
