import test from 'node:test';
import assert from 'node:assert/strict';
import {AccountError,createAccountClient} from '../public/account-client.js';
import {buildBoardOperation,createBoardModel,escapeBoard,parseBoardSnapshot,renderTaskRecord} from '../public/board-ui.js';

const sha='a'.repeat(40),nextSha='b'.repeat(40),csrf='c'.repeat(64);
const actor={id:'test-account',email:'board-user@example.test',display_name:'Board test',handle:'deandre',role:'owner',verified:true};
const task=(id='T-001',extra={})=>({id,title:'Inspect the source',status:'todo',priority:'normal',assigned_to:'deandre-fable',assigned_by:'ali',assigned_on:'2026-09-20',due:null,area:'platform',depends_on:[],instructions:'Keep the scope bounded.',log:[{on:'2026-09-20',by:'ali',text:'Created from the plan.'}],...extra});
const snapshot=()=>({sha,tasks:[task()]});
function harness({kind='tasks',failures=[],features={board:true},user=actor}={}) {
  let currentUser=user,current=kind==='tasks'?snapshot():{sha,notes:'# Notes\r\nOld content\r\n'},sequence=0;
  const calls=[];
  const model=createBoardModel({getUser:()=>currentUser,getFeatures:()=>features,uuid:()=>`00000000-0000-4000-8000-${String(++sequence).padStart(12,'0')}`,client:{async request(route,options={}) {
    calls.push({route,...structuredClone(options)});
    if(options.method!=='POST')return structuredClone(current);
    const failure=failures.shift();if(failure)throw failure;
    return {sha:nextSha,commit_sha:'d'.repeat(40),operation_id:options.body.operation_id,replayed:false,...(kind==='tasks'?{task_id:'T-001'}:{id:options.body.operation_id})};
  }}});
  return {model,calls,setCurrent:value=>current=value,setUser:value=>currentUser=value};
}

test('board client uses only exact GET/POST routes, session cookies and CSRF without Turnstile',async()=>{
  const calls=[];
  const client=createAccountClient({fetchImpl:async(url,options)=>{
    calls.push({url,options});
    return Response.json(url==='/api/me'?{user:actor,csrf,terms_required:false}:{sha,tasks:[]});
  }});
  await assert.rejects(client.request('/board/tasks',{method:'POST',body:{}}),/sign-in/);
  assert.equal(calls.length,0);
  await client.request('/me');await client.request('/board/tasks');await client.request('/board/notes',{method:'POST',body:{text:'Public test note'}});
  assert.equal(calls[1].options.method,'GET');assert.equal(calls[2].options.credentials,'same-origin');
  assert.equal(calls[2].options.headers['X-CSRF-Token'],csrf);assert.equal(JSON.parse(calls[2].options.body).turnstile,undefined);
  for(const [route,method] of [['/board/tasks','PUT'],['/board/notes','DELETE'],['/board/tasks?branch=main','GET'],['/board/README','POST']])await assert.rejects(client.request(route,{method,body:{}}),/unavailable/);
  assert.equal(calls.length,3);
});
test('task edits send changed fields and exact original non-status bases, never attribution or logs as replacements',async()=>{
  const {model,calls}=harness();await model.read('tasks');model.begin('tasks','T-001');
  model.patch('tasks','title','Inspect the final source');model.patch('tasks','status','in_progress');model.patch('tasks','assigned_to','ali-fable');model.patch('tasks','due','2026-09-28');model.patch('tasks','depends_on','T-003, T-004');model.patch('tasks','log','Scope reviewed.');
  await model.submit('tasks');const body=calls.at(-1).body;
  assert.deepEqual(body.changes,{title:'Inspect the final source',status:'in_progress',assigned_to:'ali-fable',due:'2026-09-28',depends_on:['T-003','T-004']});
  assert.deepEqual(body.base,{title:'Inspect the source',assigned_to:'deandre-fable',due:null,depends_on:[]});
  assert.equal(body.log,'Scope reviewed.');assert.equal(body.expected_sha,sha);
  for(const name of ['author','assigned_by','assigned_on','branch','path'])assert.equal(Object.hasOwn(body,name),false);
  assert.equal(Object.hasOwn(body.changes,'log'),false);assert.equal(model.getState('tasks').draft,null);
});
test('creating a task leaves status, task ID and actor to the Worker',async()=>{
  const {model,calls}=harness();await model.read('tasks');model.begin('tasks');model.patch('tasks','title','Create a clear task');model.patch('tasks','instructions','Report the verification.');
  await model.submit('tasks');const body=calls.at(-1).body;
  assert.equal(body.action,'create');assert.equal(body.task.title,'Create a clear task');assert.equal(body.task.status,undefined);assert.equal(body.task.id,undefined);assert.equal(body.task.assigned_by,undefined);assert.equal(body.task.assigned_to,null);
});
test('an unconfirmed identical retry retains its operation ID and complete payload; changed text gets a new ID',async()=>{
  const failure=()=>new AccountError('network_unavailable','Connection ended before a receipt.');
  const {model,calls}=harness({kind:'notes',failures:[failure(),failure()]});
  await model.read('notes');model.begin('notes');model.patch('notes','text','First public note.');
  await assert.rejects(model.submit('notes'),/Connection ended/);await assert.rejects(model.submit('notes'),/Connection ended/);
  const first=calls[1].body,second=calls[2].body;assert.deepEqual(second,first);assert.notEqual(model.getState('notes').draft,null);
  model.patch('notes','text','Revised public note.');await model.submit('notes');
  assert.notEqual(calls[3].body.operation_id,first.operation_id);assert.equal(calls[3].body.text,'Revised public note.');assert.deepEqual(calls[3].body.to,['all']);
});
test('a conflict preserves a draft and blocks re-save until explicit review after refresh',async()=>{
  const {model,calls,setCurrent}=harness({failures:[new AccountError('board_conflict','The board changed.',409)]});
  await model.read('tasks');model.begin('tasks','T-001');model.patch('tasks','title','My reviewed title');model.patch('tasks','log','Record this decision.');
  await assert.rejects(model.submit('tasks'),/board changed/);const failedOperation=calls.at(-1).body.operation_id;
  assert.equal(model.getState('tasks').draft.fields.title,'My reviewed title');assert.equal(model.getState('tasks').conflict.refreshed,false);
  await assert.rejects(model.submit('tasks'),/review the conflict/);assert.equal(calls.length,2);
  assert.throws(()=>model.review('tasks'),/Refresh the board/);
  setCurrent({sha:nextSha,tasks:[task('T-001',{title:'Another title',instructions:'An independent instruction update.',assigned_to:'ali',log:[...task().log,{on:'2026-09-21',by:'ali',text:'Concurrent log'}]})]});
  await model.read('tasks');assert.equal(model.getState('tasks').draft.baseTask.title,'Inspect the source');
  model.review('tasks');const reviewed=model.getState('tasks').draft;
  assert.equal(reviewed.fields.title,'My reviewed title');assert.equal(reviewed.fields.instructions,'An independent instruction update.');assert.equal(reviewed.fields.assigned_to,'ali');
  await model.submit('tasks');const saved=calls.at(-1).body;
  assert.deepEqual(saved.changes,{title:'My reviewed title'});assert.deepEqual(saved.base,{title:'Another title'});assert.equal(saved.expected_sha,nextSha);assert.notEqual(saved.operation_id,failedOperation);
});
test('ordinary refresh does not overwrite a draft or replace its original expected SHA',async()=>{
  const {model,setCurrent}=harness({kind:'notes'});await model.read('notes');model.begin('notes');model.patch('notes','text','Unsent text');
  setCurrent({sha:nextSha,notes:'Concurrent note\n'});await model.read('notes');
  const state=model.getState('notes');assert.equal(state.draft.text,'Unsent text');assert.equal(state.draft.expectedSha,sha);assert.equal(state.snapshot.notes,'Concurrent note\n');
});
test('feature flags and verified human identities gate writes; account switches and disposal clear private drafts',async()=>{
  const closed=harness({features:{board:false}});await assert.rejects(closed.model.read('tasks'),/not enabled/);assert.equal(closed.calls.length,0);
  const agent=harness({user:{...actor,handle:'deandre-fable'}});await agent.model.read('tasks');assert.throws(()=>agent.model.begin('tasks'),/verified human/);
  const {model,setUser,calls}=harness();await model.read('tasks');model.begin('tasks');model.patch('tasks','title','Local draft');
  setUser({...actor,id:'different-test-account',handle:'ali'});assert.equal(model.getState('tasks').draft,null);assert.equal(model.getState('tasks').snapshot,null);
  await assert.rejects(model.submit('tasks'),/Refresh the board/);assert.equal(calls.length,1);
  model.dispose();assert.equal(model.getState('tasks').draft,null);await assert.rejects(model.read('tasks'),/closed/);
});
test('receipts from requests started under a different account cannot repopulate its board',async()=>{
  let user=actor,resolve;
  const model=createBoardModel({client:{request:()=>new Promise(done=>resolve=done)},getUser:()=>user,getFeatures:()=>({board:true})});
  const pending=model.read('tasks');user={...actor,id:'second-test-account',handle:'ali'};resolve(snapshot());
  await assert.rejects(pending,/account changed/);assert.equal(model.getState('tasks').snapshot,null);
});
test('notes preserve stored text exactly and task history is escaped instead of interpreted as HTML',()=>{
  const content='## Note\r\n<img src=x onerror="alert(1)">\r\n  **unchanged** & text\r\n';
  assert.equal(parseBoardSnapshot('notes',{sha,notes:content}).notes,content);
  const rendered=renderTaskRecord(task('T-001',{title:'<script>alert(1)</script>',instructions:'<svg onload=x>',log:[{by:'ali-fable',on:'2026-09-21',text:'<img src=x> & "quoted"'}]}),true);
  assert.equal(rendered.includes('<script>'),false);assert.equal(rendered.includes('<svg onload'),false);assert.equal(rendered.includes('<img src=x>'),false);
  assert.match(rendered,/&lt;script&gt;/);assert.match(rendered,/ali-fable/);assert.match(rendered,/2026-09-21/);assert.equal(escapeBoard('&<>"\''),'&amp;&lt;&gt;&quot;&#39;');
});
test('request validation enforces UTF-8/JSON bounds, dates, recipients and dropped-task reasons',async()=>{
  const {model}=harness({kind:'notes'});await model.read('notes');model.begin('notes');model.patch('notes','text','Valid note');
  const id='00000000-0000-4000-8000-000000000000',note=model.getState('notes').draft;
  for(const to of [[],['all','ali'],['unknown'],['ali','ali']])assert.throws(()=>buildBoardOperation('notes',{...note,to},id),/recipients/);
  assert.throws(()=>buildBoardOperation('notes',{...note,text:'😀'.repeat(1501)},id),/6,000/);
  assert.doesNotThrow(()=>buildBoardOperation('notes',{...note,text:'"'.repeat(5000)},id));
  const tasks=harness();await tasks.model.read('tasks');tasks.model.begin('tasks','T-001');tasks.model.patch('tasks','status','dropped');
  assert.throws(()=>buildBoardOperation('tasks',tasks.model.getState('tasks').draft,id),/reason/);
  tasks.model.patch('tasks','log','No longer needed.');tasks.model.patch('tasks','due','2026-02-30');
  assert.throws(()=>buildBoardOperation('tasks',tasks.model.getState('tasks').draft,id),/due/);
});
test('malformed successful responses keep the draft and operation available for a safe retry',async()=>{
  let posts=0,body;
  const model=createBoardModel({getUser:()=>actor,getFeatures:()=>({board:true}),client:{async request(route,options={}) {
    if(options.method!=='POST')return {sha,notes:''};posts++;body=options.body;return {sha:nextSha,operation_id:'wrong',replayed:false,id:'wrong'};
  }}});
  await model.read('notes');model.begin('notes');model.patch('notes','text','Public note');
  await assert.rejects(model.submit('notes'),/not confirmed/);const first=body.operation_id;
  await assert.rejects(model.submit('notes'),/not confirmed/);assert.equal(body.operation_id,first);assert.equal(posts,2);assert.equal(model.getState('notes').draft.text,'Public note');
});


test('sixteen-KiB task instructions and their original base fit the board-only envelope',async()=>{
  const h=harness();h.setCurrent({sha,tasks:[task('T-001',{instructions:'a'.repeat(16384)})]});await h.model.read('tasks');h.model.begin('tasks','T-001');h.model.patch('tasks','instructions','b'.repeat(16384));await h.model.submit('tasks');
  const payload=h.calls.at(-1).body;assert.equal(payload.base.instructions.length,16384);assert.equal(payload.changes.instructions.length,16384);assert.ok(Buffer.byteLength(JSON.stringify(payload))>32768);assert.ok(Buffer.byteLength(JSON.stringify(payload))<65536);
  const huge=harness();huge.setCurrent({sha,tasks:[task('T-001',{instructions:'a'.repeat(65536)})]});await huge.model.read('tasks');huge.model.begin('tasks','T-001');huge.model.patch('tasks','instructions','Shortened text');
  await assert.rejects(huge.model.submit('tasks'),/64 KiB/);
});
