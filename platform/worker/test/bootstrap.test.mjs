import assert from 'node:assert/strict';
import { test } from 'node:test';
import { D1Harness } from './d1-harness.mjs';
import { prepareBootstrap, verifyBootstrapResult } from '../scripts/seed-invite.mjs';
import { digestToken } from '../security.mjs';

const accounts = [{email:'owner@example.test',handle:'deandre'},{email:'member@example.test',handle:'ali'}];
const execute = (db, prepared) => {
  db.exec(prepared.sql);
  return [{success:true,results:db.database.prepare('SELECT token_hash,email,handle,role,expires_at FROM invites WHERE token_hash=?').all(prepared.hash).map(row=>({...row}))}];
};
test('bootstrap inserts only an expiring owner invite digest and confirms the exact inserted row', t => {
  const db = new D1Harness(); t.after(()=>db.close());
  const prepared = prepareBootstrap(accounts, 1000);
  assert.equal(prepared.hash, digestToken(prepared.token)); assert.ok(!prepared.sql.includes(prepared.token));
  verifyBootstrapResult(execute(db,prepared),prepared);
  const invite = {...db.database.prepare('SELECT * FROM invites').get()};
  assert.equal(invite.role,'owner'); assert.equal(invite.handle,'deandre'); assert.equal(invite.expires_at,87400); assert.equal(invite.issued_by,null);
  assert.equal(db.database.prepare('SELECT count(*) AS n FROM users').get().n,0);
  const second=prepareBootstrap(accounts,1001);
  assert.throws(()=>verifyBootstrapResult(execute(db,second),second),/refused/);
  assert.equal(db.database.prepare('SELECT count(*) AS n FROM invites').get().n,1);
});
test('expired pending invite can be replaced but an existing account prevents bootstrap', t => {
  const db = new D1Harness(); t.after(()=>db.close());
  execute(db,prepareBootstrap(accounts,1000));
  const replacement=prepareBootstrap(accounts,87401); verifyBootstrapResult(execute(db,replacement),replacement);
  db.database.prepare("INSERT INTO users(id,email,handle,display_name,role,access_sub,access_email,password_scheme,password_iterations,password_salt,password_hash,created_at,updated_at) VALUES('u','owner@example.test','deandre','Owner','owner','subject','identity@example.test','pbkdf2-sha256',600000,'salt','hash',1,1)").run();
  const afterUser=prepareBootstrap(accounts,180000);
  assert.throws(()=>verifyBootstrapResult(execute(db,afterUser),afterUser),/refused/);
});
test('bootstrap validates the two private mappings and escapes an allowed apostrophe in the address', t => {
  const db = new D1Harness(); t.after(()=>db.close());
  for (const invalid of [[],accounts.slice(0,1),[accounts[0],accounts[0]],[accounts[0],{email:accounts[0].email,handle:'ali'}]]) assert.throws(()=>prepareBootstrap(invalid));
  const prepared=prepareBootstrap([{email:"o'owner@example.test",handle:'deandre'},accounts[1]],1000);
  verifyBootstrapResult(execute(db,prepared),prepared);
  assert.equal(db.database.prepare('SELECT email FROM invites').get().email,"o'owner@example.test");
  assert.throws(()=>verifyBootstrapResult([{success:false}],prepared),/not confirmed/);
});
