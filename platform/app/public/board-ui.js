import {AccountError,requiresSecureAccess} from './account-client.js';

export const BOARD_ROSTER=Object.freeze(['deandre','deandre-fable','deandre-gpt6','ali','ali-fable']);
export const BOARD_STATUSES=Object.freeze(['todo','in_progress','blocked','review','done','dropped']);
const PRIORITIES=['high','normal','low'],AREAS=['bot','platform','research','ops','board'];
const FIELDS=['title','status','priority','assigned_to','due','area','depends_on','instructions'];
const SHA=/^[a-f0-9]{40}$/,UUID=/^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/;
const encoder=new TextEncoder(),MAX_REQUEST_BYTES=8192;
const object=value=>value!==null&&typeof value==='object'&&!Array.isArray(value);
const clone=value=>structuredClone(value);
export const escapeBoard=value=>String(value??'').replace(/[&<>"']/g,char=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));
const fail=message=>{throw new AccountError('board_invalid_request',message);};
const same=(left,right)=>JSON.stringify(left)===JSON.stringify(right);
const stable=value=>Array.isArray(value)?value.map(stable):object(value)?Object.fromEntries(Object.keys(value).sort().map(key=>[key,stable(value[key])])):value;
const validId=value=>typeof value==='string'&&/^T-\d{3,9}$/.test(value)&&Number(value.slice(2))>0&&value===`T-${String(Number(value.slice(2))).padStart(3,'0')}`;
const validDate=value=>typeof value==='string'&&/^\d{4}-\d{2}-\d{2}$/.test(value)&&Number.isFinite(Date.parse(`${value}T00:00:00Z`))&&new Date(`${value}T00:00:00Z`).toISOString().slice(0,10)===value;
function text(value,max,empty=false) {return typeof value==='string'&&value.isWellFormed()&&(empty||Boolean(value.trim()))&&encoder.encode(value).length<=max&&!/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/u.test(value);}
function validField(key,value,incoming=true) {
  if(key==='title')return text(value,incoming?640:1048576)&&(!incoming||[...value].length<=160)&&!/[\r\n\t]/.test(value);
  if(key==='status')return BOARD_STATUSES.includes(value);
  if(key==='priority')return PRIORITIES.includes(value);
  if(key==='assigned_to')return value===null||BOARD_ROSTER.includes(value);
  if(key==='due')return value===null||validDate(value);
  if(key==='area')return AREAS.includes(value);
  if(key==='depends_on')return Array.isArray(value)&&value.length<=(incoming?100:2000)&&value.every(validId)&&new Set(value).size===value.length;
  if(key==='instructions')return text(value,incoming?6000:1048576);
  return false;
}
export function parseBoardSnapshot(kind,value) {
  if(!['tasks','notes'].includes(kind)||!object(value)||!SHA.test(value.sha||''))throw new AccountError('board_invalid_document','The board response could not be verified.');
  if(kind==='notes') {
    if(!text(value.notes,1048576,true))throw new AccountError('board_invalid_document','The note history could not be verified.');
    return {sha:value.sha,notes:value.notes};
  }
  if(!Array.isArray(value.tasks)||value.tasks.length>2000||value.tasks.some(task=>!object(task)||!validId(task.id)||FIELDS.some(key=>!validField(key,task[key],false))||!BOARD_ROSTER.includes(task.assigned_by)||!validDate(task.assigned_on)||!Array.isArray(task.log)||task.log.some(line=>!object(line)||!validDate(line.on)||!BOARD_ROSTER.includes(line.by)||!text(line.text,1048576))))throw new AccountError('board_invalid_document','The task history could not be verified.');
  if(new Set(value.tasks.map(task=>task.id)).size!==value.tasks.length)throw new AccountError('board_invalid_document','The board contains duplicate task identifiers.');
  return {sha:value.sha,tasks:clone(value.tasks)};
}
const rawFields=task=>Object.fromEntries(FIELDS.map(key=>[key,key==='depends_on'?task[key].join(', '):task[key]??'']));
const emptyTask=()=>({title:'',status:'todo',priority:'normal',assigned_to:null,due:null,area:'platform',depends_on:[],instructions:''});
function compiledFields(fields) {return Object.fromEntries(FIELDS.map(key=>[key,key==='depends_on'?String(fields[key]||'').split(/[\s,]+/).filter(Boolean):['due','assigned_to'].includes(key)?fields[key]||null:fields[key]]));}
function taskIntent(draft) {
  const fields=compiledFields(draft.fields);
  return Object.fromEntries(FIELDS.filter(key=>draft.mode==='create'?key!=='status':!same(fields[key],draft.baseTask[key])).map(key=>[key,fields[key]]));
}
export function buildBoardOperation(kind,draft,operationId) {
  if(!draft||!SHA.test(draft.expectedSha||'')||!UUID.test(operationId||''))fail('Refresh the board before saving your draft.');
  let body={expected_sha:draft.expectedSha,operation_id:operationId};
  if(kind==='notes') {
    if(!Array.isArray(draft.to)||!draft.to.length||draft.to.length>5||draft.to.some(handle=>handle!=='all'&&!BOARD_ROSTER.includes(handle))||new Set(draft.to).size!==draft.to.length||(draft.to.includes('all')&&draft.to.length!==1))fail('Choose everyone or one or more roster recipients.');
    if(!text(draft.text,6000)||draft.text.includes('<!-- platform-board-operation:')||/^## \d{4}-\d{2}-\d{2} \d{2}:\d{2} UTC · /m.test(draft.text))fail('Write a note of up to 6,000 UTF-8 bytes without forged author headers.');
    body={...body,to:[...draft.to],text:draft.text};
  } else if(kind==='tasks') {
    if(!['create','update'].includes(draft.mode))fail('Choose a task to create or edit.');
    const changes=taskIntent(draft);
    for(const [key,value] of Object.entries(changes))if(!validField(key,value))fail(`Check ${key.replaceAll('_',' ')}. Use the board's allowed values and text limits.`);
    if(draft.mode==='create')body={...body,action:'create',task:changes};
    else {
      if(!validId(draft.task_id))fail('Choose an existing task.');
      const hasLog=typeof draft.log==='string'&&Boolean(draft.log.trim());
      if(hasLog&&!text(draft.log,2000))fail('Keep the appended log to 2,000 UTF-8 bytes.');
      if(!Object.keys(changes).length&&!hasLog)fail('Change a field or append a log entry before saving.');
      if(changes.status==='dropped'&&!hasLog)fail('Add a reason to the task log before dropping this task.');
      body={...body,action:'update',task_id:draft.task_id,changes,base:Object.fromEntries(Object.keys(changes).filter(key=>key!=='status').map(key=>[key,clone(draft.baseTask[key])])),...(hasLog?{log:draft.log}:{})};
    }
  } else fail('Choose Tasks or Notes.');
  if(encoder.encode(JSON.stringify(body)).length>MAX_REQUEST_BYTES)fail('This edit exceeds the 8 KiB request limit, including its original fields. Shorten the draft or make a smaller edit.');
  return body;
}
function validateReceipt(kind,value,body) {
  if(!object(value)||!SHA.test(value.sha||'')||value.operation_id!==body.operation_id||typeof value.replayed!=='boolean'||(value.commit_sha!==undefined&&!SHA.test(value.commit_sha))||(kind==='tasks'?(!validId(value.task_id)||(body.action==='update'&&value.task_id!==body.task_id)):value.id!==body.operation_id))throw new AccountError('board_unconfirmed','The save was not confirmed. Keep this draft and retry it unchanged.');
  return value;
}
export function createBoardModel({client,getUser,getFeatures=()=>({}),uuid=()=>crypto.randomUUID()}={}) {
  if(!client?.request||typeof getUser!=='function')throw new TypeError('An existing account client and current user are required.');
  let identity=null,generation=0,disposed=false,pending=false;
  const fresh=()=>({snapshot:null,draft:null,operation:null,conflict:null,notice:''});
  let states={tasks:fresh(),notes:fresh()};
  function sync() {
    const user=getUser(),next=user?.id&&user?.handle?`${user.id}:${user.handle}`:null;
    if(next!==identity){identity=next;generation++;states={tasks:fresh(),notes:fresh()};}
    return user;
  }
  function state(kind) {sync();if(!Object.hasOwn(states,kind))fail('Choose Tasks or Notes.');return states[kind];}
  function allowed(write=false) {
    const user=sync();
    if(disposed)throw new AccountError('board_disposed','This board view is closed.');
    if(!user||user.verified!==true)throw new AccountError('login_required','Sign in to use the shared board.',401);
    if(getFeatures()?.board!==true)throw new AccountError('board_unavailable','The shared board is not enabled.',503);
    if(write&&!['deandre','ali'].includes(user.handle))throw new AccountError('board_actor_denied','Only a verified human account can write through this interface.',403);
  }
  function current(attempt) {sync();if(disposed||attempt!==generation)throw new AccountError('account_changed','The account changed; its private drafts were cleared.');}
  function begin(kind,id=null) {
    allowed(true);const item=state(kind);
    if(item.draft)fail('Save or discard the current local draft before opening another.');
    if(!item.snapshot)fail('Refresh the board before opening a draft.');
    if(kind==='notes')item.draft={expectedSha:item.snapshot.sha,text:'',to:['all']};
    else {
      const task=id?item.snapshot.tasks.find(task=>task.id===id):emptyTask();
      if(!task)fail('That task is no longer in the board. Refresh to review it.');
      item.draft={mode:id?'update':'create',task_id:id,expectedSha:item.snapshot.sha,baseTask:id?clone(task):null,fields:rawFields(task),log:''};
    }
    item.conflict=null;item.operation=null;item.notice='';return clone(item.draft);
  }
  function patch(kind,field,value) {
    allowed(true);const item=state(kind);if(!item.draft||pending)return;
    if(kind==='tasks') {if(FIELDS.includes(field))item.draft.fields[field]=String(value);else if(field==='log')item.draft.log=String(value);else fail('This task field cannot be edited.');}
    else if(field==='text')item.draft.text=String(value);else if(field==='to')item.draft.to=Array.isArray(value)?[...value]:[];else fail('This note field cannot be edited.');
  }
  function review(kind) {
    allowed(true);const item=state(kind);
    if(!item.draft||!item.conflict?.refreshed||!item.snapshot)fail('Refresh the board and review the current saved values first.');
    if(kind==='tasks'&&item.draft.mode==='update') {
      const latest=item.snapshot.tasks.find(task=>task.id===item.draft.task_id);
      if(!latest)fail('This task is no longer available. Keep your draft until you decide how to proceed.');
      const intended=taskIntent(item.draft);
      item.draft.fields=rawFields({...clone(latest),...intended});item.draft.baseTask=clone(latest);
    }
    item.draft.expectedSha=item.snapshot.sha;item.conflict=null;item.operation=null;item.notice='Draft reviewed against the refreshed board. Select Save when ready.';
  }
  return Object.freeze({
    getState(kind){const item=state(kind);return {...item,draft:item.draft?clone(item.draft):null,pending,canWrite:!disposed&&getFeatures()?.board===true&&getUser()?.verified===true&&['deandre','ali'].includes(getUser()?.handle)};},
    begin,patch,review,
    discard(kind){allowed(true);if(pending)return;const item=state(kind);item.draft=null;item.conflict=null;item.operation=null;item.notice='Local draft discarded.';},
    async read(kind) {
      allowed();const attempt=generation;
      const result=parseBoardSnapshot(kind,await client.request(`/board/${kind}`));current(attempt);
      const item=state(kind);item.snapshot=result;if(item.conflict)item.conflict.refreshed=true;return result;
    },
    async submit(kind) {
      allowed(true);if(pending)throw new AccountError('board_busy','Wait for the current save to finish.');
      const item=state(kind),attempt=generation;
      if(item.conflict)throw new AccountError('board_review_required','Refresh the board and review the conflict before saving again.',409);
      const candidate=buildBoardOperation(kind,item.draft,'00000000-0000-4000-8000-000000000000');
      delete candidate.operation_id;
      const fingerprint=JSON.stringify(stable(candidate));
      if(item.operation?.fingerprint!==fingerprint)item.operation={fingerprint,id:uuid()};
      const body=buildBoardOperation(kind,item.draft,item.operation.id);pending=true;
      try {
        const response=await client.request(`/board/${kind}`,{method:'POST',body});current(attempt);
        const result=validateReceipt(kind,response,body);
        item.draft=null;item.operation=null;item.conflict=null;item.notice=result.replayed?'The earlier save was confirmed.':'Saved to the shared project board.';
        return result;
      } catch(error) {
        current(attempt);
        if(error.status===409)item.conflict={code:error.code,message:error.message,refreshed:false};
        throw error;
      } finally {pending=false;}
    },
    dispose(){disposed=true;generation++;identity=null;states={tasks:fresh(),notes:fresh()};}
  });
}

const label=value=>value.replaceAll('_',' ');
const option=(value,current,text=label(value))=>`<option value="${escapeBoard(value)}"${value===current?' selected':''}>${escapeBoard(text)}</option>`;
function control(name,title,value,{type='text',choices=null,maxlength=6000,help='',textarea=false}={}) {
  const id=`board-${name}`,attributes=`id="${id}" name="${name}" data-board-field="${name}"${help?` aria-describedby="${id}-hint"`:''}`;
  return `<div class="board-field"><label for="${id}">${escapeBoard(title)}</label>${choices?`<select ${attributes}>${choices.map(choice=>Array.isArray(choice)?option(choice[0],value,choice[1]):option(choice,value)).join('')}</select>`:textarea?`<textarea ${attributes} rows="5" maxlength="${maxlength}">${escapeBoard(value)}</textarea>`:`<input ${attributes} type="${type}" value="${escapeBoard(value)}" maxlength="${maxlength}"${name==='title'?' required':''}>`}${help?`<p class="field-hint" id="${id}-hint">${escapeBoard(help)}</p>`:''}</div>`;
}
function taskEditor(draft) {
  const f=draft.fields;
  return `<div class="board-form-grid">${control('title','Task title',f.title,{maxlength:160})}${draft.mode==='update'?control('status','Status',f.status,{choices:BOARD_STATUSES}):'<p class="board-help">New tasks start as todo. The server assigns the task number and author.</p>'}${control('priority','Priority',f.priority,{choices:PRIORITIES})}${control('assigned_to','Assign to',f.assigned_to,{choices:[['','Unclaimed'],...BOARD_ROSTER]})}${control('due','Due date (UTC)',f.due,{type:'date'})}${control('area','Area',f.area,{choices:AREAS})}</div>${control('depends_on','Dependencies',f.depends_on,{maxlength:1400,help:'Comma-separated task IDs, such as T-001, T-003. Dependencies must be done before work advances.'})}${control('instructions','Instructions',f.instructions,{textarea:true,help:'Describe the outcome and scope. Up to 6,000 UTF-8 bytes; no private account information.'})}${draft.mode==='update'?control('log','Append to task log',draft.log,{textarea:true,maxlength:2000,help:'Optional context, up to 2,000 UTF-8 bytes. A reason is required when dropping a task. Existing log entries stay unchanged.'}):''}`;
}
function noteEditor(draft) {
  return `<fieldset class="board-recipients"><legend>Recipients</legend>${['all',...BOARD_ROSTER].map(handle=>`<label><input type="checkbox" name="to" value="${handle}" data-board-recipient${draft.to.includes(handle)?' checked':''}>${handle==='all'?'Everyone':handle}</label>`).join('')}</fieldset>${control('text','Append a note',draft.text,{textarea:true,help:'Up to 6,000 UTF-8 bytes. The server adds your verified handle and UTC time. Notes are append-only.'})}`;
}
export function renderTaskRecord(task,canEdit=false) {
  return `<article class="panel board-task"><div class="panel-head"><div><h2>${escapeBoard(task.id)} · ${escapeBoard(task.title)}</h2><p>${escapeBoard(label(task.status))} · ${escapeBoard(task.priority)} priority · ${escapeBoard(task.area)}</p></div>${canEdit?`<button class="secondary" type="button" data-board-edit="${escapeBoard(task.id)}">Edit</button>`:''}</div><div class="panel-body"><dl class="board-metadata"><div><dt>Assigned to</dt><dd>${escapeBoard(task.assigned_to||'Unclaimed')}</dd></div><div><dt>Assigned by</dt><dd>${escapeBoard(task.assigned_by)} · ${escapeBoard(task.assigned_on)}</dd></div><div><dt>Due (UTC)</dt><dd>${escapeBoard(task.due||'No date')}</dd></div><div><dt>Dependencies</dt><dd>${escapeBoard(task.depends_on.join(', ')||'None')}</dd></div></dl><details><summary>Instructions and history (${task.log.length})</summary><p class="board-plain">${escapeBoard(task.instructions)}</p><ol class="board-log">${task.log.map(line=>`<li><strong>${escapeBoard(line.by)}</strong><time>${escapeBoard(line.on)}</time><p>${escapeBoard(line.text)}</p></li>`).join('')}</ol></details></div></article>`;
}
function conflictView(state,kind) {
  if(!state.conflict)return '';
  let comparison='';
  if(state.conflict.refreshed&&kind==='tasks'&&state.draft?.mode==='update') {
    const current=state.snapshot.tasks.find(task=>task.id===state.draft.task_id);
    const intended=taskIntent(state.draft);
    comparison=current?`<div class="board-review"><h3>Current saved values for fields in your draft</h3>${Object.keys(intended).map(key=>`<div><strong>${escapeBoard(label(key))}</strong><pre>${escapeBoard(Array.isArray(current[key])?current[key].join(', '):current[key]??'Unassigned')}</pre></div>`).join('')}</div>`:'<p>The edited task is no longer in the current board.</p>';
  }
  return `<section class="board-conflict" role="alert"><h3>Review this draft before saving again</h3><p>${escapeBoard(state.conflict.message||'The board changed.')} Your local draft is preserved.</p><p>Refresh to load the current board. Review the saved values and your draft, then explicitly use that version.</p>${comparison}<div class="board-actions"><button type="button" class="secondary" data-board-action="refresh">Refresh board for review</button><button type="button" class="secondary" data-board-action="review"${state.conflict.refreshed?'':' disabled'}>Use refreshed version for this draft</button></div></section>`;
}
export function createBoardUI({client,getUser,getFeatures=()=>({}),onAuthError=()=>{}}={}) {
  const model=createBoardModel({client,getUser,getFeatures});
  let root=null,kind='tasks',revision=0,disposed=false,message='',loading=false;
  function owned(element){return root?.contains(element);}
  function authError(error){if(requiresSecureAccess(error)||error.status===401||error.code==='terms_required'){onAuthError(error);return true;}return false;}
  function errorText(error){return error instanceof AccountError?error.message:'The board request did not complete. No save has been confirmed.';}
  function paint() {
    if(!root||disposed)return;
    const state=model.getState(kind),enabled=getFeatures()?.board===true,user=getUser();
    root.innerHTML=`<div class="board-shell" data-board-shell><p class="board-public-notice"><strong>Shared in the public project repository.</strong> Keep passwords, email addresses, account details and other private information out of tasks and notes. ${user?.verified===true?`Your author handle is <strong>${escapeBoard(user.handle)}</strong>; the server supplies attribution.`:''}</p><div class="board-toolbar"><p>${loading?'Loading the current board…':state.snapshot?`Last loaded board version ${escapeBoard(state.snapshot.sha.slice(0,8))}.`:'No board version has loaded.'}</p><button class="secondary" type="button" data-board-action="refresh"${loading||state.pending||!enabled?' disabled':''}>Refresh board</button>${enabled&&state.canWrite&&!state.draft?`<button class="primary" type="button" data-board-action="new"${loading||state.pending?' disabled':''}>${kind==='tasks'?'New task':'Write a note'}</button>`:''}</div><p class="board-feedback" role="status" aria-live="polite">${escapeBoard(message||state.notice)}</p>${!enabled?'<section class="panel empty-state"><h2>The shared board is not enabled</h2><p>Read and write controls require the configured account service.</p></section>':`${conflictView(state,kind)}${state.draft?`<section class="panel board-draft"><div class="panel-head"><h2>${kind==='notes'?'New note':state.draft.mode==='create'?'New task':`Edit ${escapeBoard(state.draft.task_id)}`}</h2><span class="tag">LOCAL DRAFT</span></div><form class="panel-body board-form" data-board-form aria-busy="${state.pending}"><fieldset${state.pending?' disabled':''}><legend class="board-visually-hidden">${kind==='tasks'?'Task draft':'Note draft'}</legend>${kind==='tasks'?taskEditor(state.draft):noteEditor(state.draft)}<p class="board-help">This draft stays in this tab until submitted. Refreshing this page, switching accounts or closing it clears the local draft. A complete request is limited to 8 KiB.</p><div class="board-actions"><button type="submit" class="primary"${state.conflict||!state.canWrite?' disabled':''}>${state.pending?'Saving…':kind==='notes'?'Append note':'Save task'}</button><button type="button" class="secondary" data-board-action="discard">Discard local draft</button></div></fieldset></form></section>`:''}<div data-board-records>${state.snapshot?(kind==='tasks'?state.snapshot.tasks.map(task=>renderTaskRecord(task,state.canWrite&&!state.draft&&!state.pending)).join('')||'<section class="panel empty-state"><h2>No tasks have been recorded</h2></section>':'<section class="panel"><div class="panel-head"><h2>Shared note history</h2><span class="tag">PLAIN TEXT · NEWEST FIRST</span></div><div class="panel-body"><pre class="board-notes" data-board-notes></pre></div></section>'):''}</div>`}</div>`;
    // Preserve note content as text. Repository Markdown is never interpreted as HTML.
    const notes=root.querySelector('[data-board-notes]');if(notes&&state.snapshot)notes.textContent=state.snapshot.notes;
  }
  async function refresh() {
    const attempt=revision;loading=true;paint();
    try {await model.read(kind);if(attempt===revision)message='';}
    catch(error){if(attempt===revision&&!authError(error))message=errorText(error);}
    finally {if(attempt===revision){loading=false;paint();}}
  }
  function focusDraft(){root?.querySelector('[data-board-field]')?.focus();}
  function input(event) {
    if(!owned(event.target))return;
    try {
      if(!model.getState(kind).draft){paint();return;}
      if(event.target.matches('[data-board-recipient]')) {
        const checkboxes=[...root.querySelectorAll('[data-board-recipient]')];
        if(event.target.checked){if(event.target.value==='all')checkboxes.forEach(box=>{if(box.value!=='all')box.checked=false;});else checkboxes.find(box=>box.value==='all').checked=false;}
        model.patch(kind,'to',checkboxes.filter(box=>box.checked).map(box=>box.value));
      } else if(event.target.matches('[data-board-field]'))model.patch(kind,event.target.dataset.boardField,event.target.value);
    } catch(error){if(!authError(error)){message=errorText(error);paint();}}
  }
  async function click(event) {
    const target=event.target.closest('[data-board-action],[data-board-edit]');if(!target||!owned(target))return;
    const action=target.dataset.boardAction;
    try {
      message='';
      if(action==='refresh'){await refresh();return;}
      if(action==='new'||target.dataset.boardEdit){model.begin(kind,target.dataset.boardEdit||null);paint();focusDraft();return;}
      if(action==='discard')model.discard(kind);
      if(action==='review')model.review(kind);
      paint();
    } catch(error){if(!authError(error)){message=errorText(error);paint();}}
  }
  async function submit(event) {
    if(!event.target.matches('[data-board-form]')||!owned(event.target))return;
    event.preventDefault();const attempt=revision,submittedKind=kind;
    try {
      message='';const promise=model.submit(kind);paint();await promise;
      if(attempt!==revision||disposed)return;
      await refresh();
    } catch(error) {
      if(attempt!==revision||disposed||authError(error))return;
      message=error.status===409?errorText(error):`${errorText(error)} Your local draft is preserved. If the save is unconfirmed, retry it unchanged to check the same operation.`;
      if(submittedKind===kind)paint();
    }
  }
  function detach(){if(!root)return;root.removeEventListener('click',click);root.removeEventListener('input',input);root.removeEventListener('change',input);root.removeEventListener('submit',submit);if(root.querySelector('[data-board-shell]'))root.replaceChildren();}
  return Object.freeze({
    async mount(target,nextKind) {
      if(disposed)throw new Error('This board view is disposed.');
      if(!['tasks','notes'].includes(nextKind))throw new TypeError('Board kind must be tasks or notes.');
      revision++;detach();root=target;kind=nextKind;message='';loading=false;
      root.addEventListener('click',click);root.addEventListener('input',input);root.addEventListener('change',input);root.addEventListener('submit',submit);
      paint();if(getFeatures()?.board===true)await refresh();
    },
    dispose(){disposed=true;revision++;detach();model.dispose();root=null;message='';}
  });
}
