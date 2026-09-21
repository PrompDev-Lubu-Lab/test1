import test from 'node:test';
import assert from 'node:assert/strict';
import { heartbeatFreshness } from '../public/freshness.js';

test('heartbeat turns stale at 45 seconds even if no further network response arrives',()=>{
  const instance={heartbeat:{age_seconds:10,status:'healthy'},_receivedAtMs:1000};
  assert.equal(heartbeatFreshness(instance,35999).kind,'recent');
  assert.equal(heartbeatFreshness(instance,36000).kind,'stale');
  assert.equal(heartbeatFreshness(instance,96000).age,105);
});
test('unconfirmed timestamps and synthetic snapshots are never presented as a current heartbeat',()=>{
  assert.equal(heartbeatFreshness({},1).kind,'unknown');
  assert.equal(heartbeatFreshness({heartbeat:{age_seconds:-1},_receivedAtMs:0},1).kind,'unknown');
  const instance={heartbeat:{age_seconds:0},_receivedAtMs:2000};
  assert.equal(heartbeatFreshness(instance,1000).age,0);
  assert.equal(heartbeatFreshness(instance,999999,true).kind,'synthetic');
});
