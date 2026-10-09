'use strict';
const workspace = {tab: 'install', folder: null, selected: null, loading: false, action: null, slotChosen: false,
  dismissed: setting('dismissedActivity'), localJob: null, serverJobId: '', download: null};
const activeJob = () => state.busy || ['prepared','uploading','ready','queued','installing'].includes(state.job?.phase);
const icon = name => {
  const svg = document.createElementNS('http://www.w3.org/2000/svg','svg');
  const use = document.createElementNS(svg.namespaceURI,'use');
  svg.setAttribute('class','icon'); use.setAttribute('href','#'+name); svg.append(use); return svg;
};
function showTab(tab) {
  if (workspace.tab !== tab && ['completed','failed','cancelled'].includes(state.job?.phase)) clearActivity();
  workspace.tab = tab;
  $('workspace').firstElementChild.hidden = tab !== 'install';
  for (const name of ['root','files','device']) $(name+'Panel').hidden = tab !== name;
  for (const button of $('workspaceTabs').children) {
    const selected = button.dataset.tab === tab;
    button.classList.toggle('selected',selected); button.setAttribute('aria-pressed',String(selected));
  }
  if (tab === 'files' && !workspace.folder && $('fileStorage').value) loadFolder($('fileStorage').value);
  activityVisibility();
}
function activityVisibility() {
  const empty = !state.busy && (!state.job || state.job.phase === 'idle');
  const hidden = empty && workspace.tab !== 'install';
  $('workspace').children[1].hidden = hidden;
  $('workspace').classList.toggle('single-pane',hidden);
}
function clearActivity() {
  if (state.job?.id&&!workspace.localJob) {workspace.dismissed=state.job.id;store('dismissedActivity',workspace.dismissed)}
  workspace.localJob=null;
  originalUpdateJob({phase:'idle'});$('log').textContent='No active operation.';
  $('installerDetails')?.setAttribute('hidden','');notice('');activityVisibility();
}
const originalConnected = connected;
connected = function(yes) { originalConnected(yes); $('workspaceTabs').hidden = !yes; if (yes) showTab(workspace.tab); };
const originalControls = controls;
controls = function() {
  originalControls();
  if(workspace.download){$('cancel').hidden=false;$('cancel').disabled=false;$('cancel').textContent='Cancel download';}
  else $('cancel').textContent='Discard transfer';
  const imageTarget=state.info?.partitions.find(partition=>partition.path===$('partition').value);
  if(imageTarget&&!imageTarget.slot_select){$('slots').disabled=true;$('slots').value='current';}
  const blocked = !state.connected || activeJob() || !!state.info?.busy;
  for (const id of ['rootInspect','rootRefresh','rootPatch','rootRollback','rootManager'])
    $(id).disabled = blocked || !state.info?.root_available || !!state.info?.locked || !$('rootSlot').value;
  for (const button of document.querySelectorAll('[data-reboot]')) button.disabled = blocked;
  for (const id of ['fileUpload','folderNew','folderUp','folderRefresh','fileRename','fileDelete','fileDownload','fileInstall'])
    $(id).disabled = blocked || workspace.loading || !workspace.folder || (id.startsWith('file') && id !== 'fileUpload' && !workspace.selected);
  $('fileStorage').disabled = blocked || workspace.loading;
  if (state.saveFolder) {
    $('imageFields').hidden = true; $('storage').disabled = true;
    $('transfer').disabled = !state.connected || !state.file || activeJob() || !!state.info?.busy;
  } else $('transfer').disabled = $('transfer').disabled || !!state.info?.busy;
  if (state.job?.save_path) $('install').textContent = 'Save file';
  renderPackageInfo($('packageInfo'),!state.file||state.job?.name===state.file.name?state.job?.package_info:null);
  const packageInfo=state.job?.package_info;
  if(packageInfo?.error||packageInfo?.arb_decision==='downgrade') $('install').disabled=true;
  activityVisibility();
};
const originalUpdateInfo = updateInfo;
updateInfo = function(info) {
  originalUpdateInfo(info);
  $('workspaceTabs').hidden = !state.connected;
  options($('fileStorage'),info.storages||[],s=>`${s.name} / ${size(s.free)} free`);
  const slotSignature = `${info.boot_slot}|${info.slot}`;
  if ($('rootSlot').dataset.signature !== slotSignature) {
    const selected = $('rootSlot').value; $('rootSlot').replaceChildren();
    const active = (info.slot||'').toLowerCase().replace(/^_/,'') || info.boot_slot || '';
    $('rootSlot').add(new Option('Select slot',''));
    for (const slot of ['a','b']) $('rootSlot').add(new Option(`Slot ${slot.toUpperCase()}${slot===active?' / active':' / inactive'}${slot===info.boot_slot?' / running recovery':''}`,slot));
    $('rootSlot').value = workspace.slotChosen && selected ? selected : active;
    $('rootSlot').dataset.signature = slotSignature;
  }
  $('rootTarget').textContent = $('rootSlot').value ? 'init_boot_'+$('rootSlot').value : 'init_boot';
  $('rootUnavailable').hidden = !!info.root_available && !info.locked;
  $('rootUnavailable').textContent = info.locked ? 'Unlock internal storage for verified rollback backups.' :
    'This device does not expose the init_boot / KernelSU patching backend. Root is unavailable; other tools remain usable.';
  const memory = /MemAvailable:\s+(\d+) kB/.exec(info.memory||'');
  const total = /MemTotal:\s+(\d+) kB/.exec(info.memory||'');
  const uptime = Math.floor(parseFloat(info.uptime||'0') / 60);
  const values = [
    ['Device',info.device],['Version',info.version],['Kernel',info.kernel],['Architecture',info.architecture],
    ['Recovery slot',info.boot_slot?.toUpperCase()||'Unknown'],['Selected slot',info.slot||'--'],
    ['Storage',info.locked?'Locked':'Unlocked'],['Battery',info.battery?`${info.battery}% / ${info.battery_status||''}`:'Unavailable'],
    ['Memory',memory&&total?`${size(Number(memory[1])*1024)} available / ${size(Number(total[1])*1024)}`:'Unavailable'],
    ['Uptime',`${Math.floor(uptime/60)}h ${uptime%60}m`],['Address',info.address||info.ip_address]
  ];
  const details = $('deviceDetails'); details.replaceChildren();
  for (const [label,value] of values) {
    const row=document.createElement('div'),term=document.createElement('dt'),detail=document.createElement('dd');
    term.textContent=label;detail.textContent=value||'--';row.append(term,detail);details.append(row);
  }
  $('certificateFingerprint').textContent = (info.certificate||'').match(/.{1,2}/g)?.join(':')||'--';
  $('certificateState').textContent = info.persistent_certificate ? 'Private device authority / SHA-256 fingerprint' :
    'Temporary authority: writable persistent data is unavailable. Do not permanently trust this session certificate.';
  controls();
};
const originalUpdateJob = updateJob;
updateJob = function(job) {
  if (state.busy) return;
  if(workspace.localJob){
    if(job.id&&job.id!==workspace.serverJobId&&job.id!==workspace.localJob.id)workspace.localJob=null;
    else job=workspace.localJob;
  }
  if (job.id===workspace.dismissed && ['completed','failed','cancelled'].includes(job.phase)) job={phase:'idle'};
  originalUpdateJob(job);
  if (job.root_action) {
    $('progressLabel').textContent = 'Root operation';
    $('statusTitle').textContent = job.phase === 'completed' ? 'Root operation complete' : job.phase === 'failed' ? 'Root operation failed' : job.phase === 'queued' ? 'Root operation queued' : 'Working on root';
    if (job.phase === 'completed') notice('Root operation completed.','success');
  } else if (job.save_path) {
    $('statusTitle').textContent = job.phase === 'ready' ? 'Ready to save' : job.phase === 'completed' ? 'File saved' : $('statusTitle').textContent;
    if (job.phase === 'completed') notice(job.detail,'success');
  } else if(job.download_name){
    $('progressLabel').textContent='Download';
    $('statusTitle').textContent=job.phase==='completed'?'Download complete':'Download interrupted';
    notice(job.detail,job.phase==='completed'?'success':'');
  }
  renderInstallerDetails(job.presentation);
  activityVisibility();
  controls();
};
const originalSelectFile = selectFile;
selectFile = function(file) {
  if (!activeJob()) state.saveFolder = '';
  originalSelectFile(file);
  if(state.file===file&&file&&/\.img$/i.test(file.name)){
    const name=file.name.toLowerCase().replace(/\.img$/,'');
    const target=state.info?.partitions.find(partition=>partition.path.split('/').pop()===name);
    if(target)$('partition').value=target.path;
    controls();
  }
};
const originalConfirmation = showConfirmation;
showConfirmation = function() {
  originalConfirmation();
  if (state.job?.save_path && state.job.phase === 'ready') {
    $('confirmTitle').textContent = 'Save verified file?'; $('confirmInstall').textContent = 'Save file';
    $('review').replaceChildren();reviewLine('File',state.job.name);reviewLine('Size',size(state.job.bytes));reviewLine('Destination',state.job.save_path);
    $('confirmWarning').textContent = 'Save this file without flashing it. Existing files are never overwritten.';
  } else $('confirmWarning').textContent = 'The file has been transferred and verified. Installation will run on the device.';
  const info=state.job?.package_info;
  if(state.job?.partition&&!state.info?.partitions.find(target=>target.path===state.job.partition)?.slot_select){
    for(const row of $('review').children)if(row.firstElementChild.textContent==='Slots')row.lastElementChild.textContent='Single partition';
  }
  const imageWarning=state.job?.partition?'Flashing overwrites the selected partition. A wrong image or firmware rollback can leave the device unbootable.':'';
  if(imageWarning)$('confirmWarning').textContent=imageWarning;
  renderPackageInfo($('confirmPackage'),info);
  if(info?.is_payload) $('confirmTitle').textContent='Review Android update';
  $('arbAcknowledged').checked=false;$('arbConsent').hidden=info?.arb_decision!=='upgrade';
  $('confirmInstall').disabled=!!info?.error||info?.arb_decision==='downgrade'||info?.arb_decision==='upgrade';
  if(state.job?.source_path){
    reviewLine('Source',state.job.source_path);
    $('confirmWarning').textContent=(imageWarning?imageWarning+'\n\n':'')+'This file is already on the device. Installation will use it directly and will not delete it afterward.';
  }
};
$('install').onclick = showConfirmation;
const originalDiscard=discard;
$('cancel').onclick=()=>{if(workspace.download)workspace.download.abort();else originalDiscard()};
$('arbAcknowledged').onchange=()=>{$('confirmInstall').disabled=!$('arbAcknowledged').checked};
$('confirmInstall').onclick=async()=>{
  if(state.job?.phase!=='ready'||$('confirmInstall').disabled)return;
  $('confirmInstall').disabled=true;
  try{updateJob(await api('install',{id:state.job.id,confirm:true,arb_acknowledged:$('arbAcknowledged').checked}));$('confirm').close();notice('')}
  catch(error){notice(error.message);$('confirm').close()}
  finally{$('confirmInstall').disabled=false}
};
function confirmation(title,text,run,name) {
  workspace.action = run; $('actionTitle').textContent=title; $('actionText').textContent=text;
  $('actionField').hidden = name === undefined; $('actionName').value = name||'';
  $('actionRun').disabled = false; $('actionRun').textContent = 'Confirm'; $('actionConfirm').showModal();
  if (name !== undefined) { $('actionName').focus(); $('actionName').select(); }
}
async function runAction() {
  if (!workspace.action || $('actionRun').disabled) return;
  $('actionRun').disabled=true;
  try { await workspace.action($('actionName').value.trim()); $('actionConfirm').close(); }
  catch(error) { $('actionConfirm').close(); notice(error.message); }
  finally { workspace.action=null; $('actionRun').disabled=false; await refresh().catch(()=>{}); }
}
$('actionRun').onclick=runAction;$('actionCancel').onclick=()=>$('actionConfirm').close();
$('actionName').onkeydown=event=>{if(event.key==='Enter'){event.preventDefault();runAction()}};
for(const button of $('workspaceTabs').children) button.onclick=()=>showTab(button.dataset.tab);
$('rootSlot').onchange=()=>{ workspace.slotChosen=true;$('rootTarget').textContent='init_boot_'+$('rootSlot').value;controls(); };
async function rootOperation(action) {
  const slot=$('rootSlot').value,provider=$('rootProvider').value;
  const run=async()=>{notice('');updateJob(await api('root',{action,slot,provider,confirm:true}));};
  if(action==='inspect'||action==='refresh') {try{await run()}catch(error){notice(error.message)}return}
  const providerName=$('rootProvider').selectedOptions[0].textContent;
  const text=action==='patch'?`${providerName}\nTarget: init_boot_${slot}\n\nThe target boot kernel must match an exact module. AERA saves a verified full rollback image before writing. This does not switch slots or reboot.`:
    action==='rollback'?`Restore the latest verified AERA backup to init_boot_${slot}. This overwrites the selected partition.`:
    `${providerName}\nDownload and stage the manager APK for installation when Android starts. This does not patch a partition.`;
  confirmation(action==='patch'?'Patch selected slot?':action==='rollback'?'Restore root backup?':'Install manager APK?',text,run);
}
for(const [id,action] of [['rootInspect','inspect'],['rootRefresh','refresh'],['rootPatch','patch'],['rootRollback','rollback'],['rootManager','manager']]) $(id).onclick=()=>rootOperation(action);
for(const button of document.querySelectorAll('[data-reboot]')) button.onclick=()=>{
  const target=button.dataset.reboot;
  confirmation(target==='poweroff'?'Power off device?':`Reboot to ${button.textContent.trim()}?`,
    'The connection will close. No slot is changed by this command.',async()=>{
      await api('reboot',{target,confirm:true});notice(target==='poweroff'?'Power off requested.':'Reboot requested.','success');
    });
};
async function loadFolder(path,append=false) {
  if(workspace.loading)return;workspace.loading=true;controls();
  if(!append){$('fileList').replaceChildren();const waiting=document.createElement('div');waiting.className='file-empty';waiting.textContent='Loading folder...';$('fileList').append(waiting);workspace.selected=null;$('fileSelection').hidden=true;}
  try{
    const result=await api('files/list',{path,offset:append?workspace.folder?.next||0:0});
    workspace.folder=result;$('folderPath').textContent=result.path;$('fileCount').textContent=result.total+' entries';
    if(!append)$('fileList').replaceChildren();
    for(const entry of result.entries){
      const row=document.createElement('button');row.className='file-row';row.disabled=!entry.directory&&!entry.regular;
      const text=document.createElement('div');text.style.minWidth='0';
      const name=document.createElement('div');name.className='entry-name';name.textContent=entry.name;
      const meta=document.createElement('div');meta.className='entry-meta';
      meta.textContent=(entry.directory?'Folder':entry.regular?size(entry.bytes):'Special / link')+' / '+new Date(entry.modified*1000).toLocaleString();
      text.append(name,meta);row.append(icon(entry.directory?'folder':'file'),text);
      row.onclick=()=>{if(entry.directory)loadFolder(entry.path);else selectEntry(entry,row)};
      row.oncontextmenu=event=>{event.preventDefault();selectEntry(entry,row)};
      $('fileList').append(row);
    }
    if(!result.total){const empty=document.createElement('div');empty.className='file-empty';empty.textContent='Empty folder';$('fileList').append(empty);}
    $('filesMore').hidden=result.next===null;$('folderUp').disabled=result.path===result.root;
    if(result.truncated)notice('This folder has more than 10,000 entries. Only the first scanned entries are listed.','');
  }catch(error){notice(error.message);if(!append)$('fileList').replaceChildren();}
  finally{workspace.loading=false;controls();if(workspace.folder)$('folderUp').disabled=$('folderUp').disabled||workspace.folder.path===workspace.folder.root;}
}
function selectEntry(entry,row){
  workspace.selected=entry;for(const child of $('fileList').children)child.classList.toggle('selected',child===row);
  $('fileSelection').hidden=false;$('selectedName').textContent=entry.name;
  $('selectedDetails').textContent=entry.directory?'Empty folders can be deleted.':size(entry.bytes)+' / '+new Date(entry.modified*1000).toLocaleString();
  $('fileDownload').hidden=entry.directory;
  $('fileInstall').hidden=!entry.regular||!entry.bytes||!(/\.(zip|img)$/i.test(entry.name));
  $('fileInstallLabel').textContent=/\.img$/i.test(entry.name)?'Flash image':'Install ZIP';controls();
}
async function prepareExistingFile(entry,image={}){
  if(!entry||activeJob())return;
  state.busy=true;controls();notice('Reading package information...');
  try{
    const job=await api('files/prepare',{path:entry.path,...image});
    state.busy=false;state.file=null;state.saveFolder='';state.saveRoot='';
    showTab('install');
    $('fileInfo').hidden=false;$('fileName').textContent=entry.name;
    $('fileDetails').textContent=size(entry.bytes)+' / On device';$('imageFields').hidden=true;
    updateJob(job);notice('');showConfirmation();
  }catch(error){notice(error.message)}
  finally{state.busy=false;controls()}
}
$('fileInstall').onclick=()=>{
  const entry=workspace.selected;if(!entry||activeJob())return;
  if(!/\.img$/i.test(entry.name)){prepareExistingFile(entry);return;}
  $('localImageName').textContent=entry.name;
  options($('localImagePartition'),state.info.partitions||[],partition=>partition.name,'Select partition');
  const name=entry.name.toLowerCase().replace(/\.img$/,'');
  $('localImagePartition').value=state.info.partitions.find(partition=>partition.path.split('/').pop()===name)?.path||'';
  $('localImageSlots').value='current';updateLocalImageTarget();$('localImageConfirm').showModal();
};
function updateLocalImageTarget(){
  const partition=state.info?.partitions.find(item=>item.path===$('localImagePartition').value);
  $('localImageReview').disabled=!partition;
  $('localImageSlots').disabled=!!partition?.logical||!partition?.slot_select;
  if($('localImageSlots').disabled)$('localImageSlots').value='current';
}
$('localImagePartition').onchange=updateLocalImageTarget;
$('localImageCancel').onclick=()=>$('localImageConfirm').close();
$('localImageReview').onclick=()=>{
  if($('localImageReview').disabled)return;
  const target={partition:$('localImagePartition').value,both_slots:$('localImageSlots').value==='both'};
  $('localImageConfirm').close();prepareExistingFile(workspace.selected,target);
};
$('fileStorage').onchange=()=>{workspace.folder=null;loadFolder($('fileStorage').value)};
$('folderRefresh').onclick=()=>loadFolder(workspace.folder.path);$('folderUp').onclick=()=>loadFolder(workspace.folder.parent);
$('filesMore').onclick=()=>loadFolder(workspace.folder.path,true);
$('folderNew').onclick=()=>confirmation('New folder',workspace.folder.path,async name=>{await api('files/mkdir',{path:workspace.folder.path,name,confirm:true});await loadFolder(workspace.folder.path)},'');
$('fileRename').onclick=()=>{const entry=workspace.selected;confirmation('Rename',entry.name,async name=>{await api('files/rename',{path:entry.path,name,confirm:true});await loadFolder(workspace.folder.path)},entry.name)};
$('fileDelete').onclick=()=>{const entry=workspace.selected;confirmation('Delete '+(entry.directory?'empty folder?':'file?'),entry.path+'\n\nThis cannot be undone.',async()=>{await api('files/delete',{path:entry.path,confirm:true});await loadFolder(workspace.folder.path)})};
$('fileDownload').onclick=async()=>{
  const entry=workspace.selected;if(!entry||state.busy)return;
  let writer=null,watchdog=null;
  const abort=new AbortController();
  try{
    if(entry.bytes>64*1024*1024){
      if(typeof window.showSaveFilePicker!=='function'){
        confirmation('Download large file?',entry.name+'\n\nThis browser cannot stream into a local file. Import the recovery certificate first, then continue with a normal browser download.',async()=>{
          const result=await api('files/download',{path:entry.path});const link=document.createElement('a');link.href=result.url;link.download=entry.name;link.click();
          notice('Download sent to your browser. Check its Downloads panel for certificate warnings.','success');
        });return;
      }
      const handle=await window.showSaveFilePicker({suggestedName:entry.name});
      writer=await handle.createWritable();
    }
    const serverJobId=workspace.localJob?workspace.serverJobId:state.job?.id||'';
    if(['completed','failed','cancelled'].includes(state.job?.phase))clearActivity();
    workspace.serverJobId=serverJobId;workspace.download=abort;
    state.busy=true;controls();activityVisibility();notice('');
    $('statusTitle').textContent='Downloading file';$('statusDetail').textContent=entry.name;
    $('progressLabel').textContent='Download';$('progress').value=0;$('percent').textContent='0%';
    const result=await api('files/download',{path:entry.path});
    let lastProgress=performance.now();const started=lastProgress;
    watchdog=setInterval(()=>{if(performance.now()-lastProgress>60000)abort.abort()},5000);
    const response=await fetch(result.url,{headers:{Authorization:'Bearer '+state.token},signal:abort.signal,cache:'no-store'});
    if(!response.ok){let error;try{error=await response.json()}catch{}throw new Error(error?.error||'Download failed.');}
    const reader=response.body.getReader();const chunks=[];let received=0;
    for(;;){
      const {value,done}=await reader.read();if(done)break;
      received+=value.length;
      if(received>entry.bytes||(!writer&&received>64*1024*1024)){
        await reader.cancel();throw new Error('The file changed during the download. Please refresh the folder.');
      }
      if(writer)await writer.write(value);else chunks.push(value);
      lastProgress=performance.now();const percent=entry.bytes?Math.min(100,100*received/entry.bytes):100;
      $('progress').value=percent;$('percent').textContent=Math.floor(percent)+'%';
      $('bytes').textContent=size(received)+' / '+size(entry.bytes);
      $('speed').textContent=size(received/Math.max(.1,(lastProgress-started)/1000))+'/s';
    }
    if(received!==entry.bytes)throw new Error('The file changed or the download was incomplete.');
    if(writer){await writer.close();writer=null;}
    else{const url=URL.createObjectURL(new Blob(chunks,{type:'application/octet-stream'}));const link=document.createElement('a');link.href=url;link.download=entry.name;link.click();setTimeout(()=>URL.revokeObjectURL(url),30000);}
    state.busy=false;
    workspace.localJob={id:'download-'+crypto.randomUUID(),phase:'completed',download_name:entry.name,
      detail:'Downloaded '+entry.name+'.',bytes:entry.bytes,received:entry.bytes,progress:100,
      log:'Downloaded '+entry.name+' to this computer.'};
    updateJob(workspace.localJob);
  }catch(error){
    const message=error.name==='AbortError'?'Download interrupted.':error.message;
    if(workspace.download){
      state.busy=false;workspace.localJob={id:'download-'+crypto.randomUUID(),phase:'failed',download_name:entry.name,detail:message};
      updateJob(workspace.localJob);
    }else if(error.name!=='AbortError')notice(message);
  }
  finally{
    clearInterval(watchdog);if(writer)await writer.abort().catch(()=>{});
    workspace.download=null;state.busy=false;controls();activityVisibility();
  }
};
$('fileUpload').onclick=()=>$('storageFile').click();
$('storageFile').onchange=async()=>{
  const file=$('storageFile').files[0];if(!file||activeJob()||!workspace.folder)return;
  const destination=workspace.folder.path;
  state.file=file;state.saveFolder=workspace.folder.path;state.saveRoot=workspace.folder.root;
  $('fileInfo').hidden=false;$('fileName').textContent=file.name;$('fileDetails').textContent=size(file.size)+' / Save to '+state.saveFolder;
  $('imageFields').hidden=true;notice('');controls();$('storageFile').value='';
  await transfer();
  if(state.job?.phase==='ready'&&state.job.save_path){
    try{updateJob(await api('install',{id:state.job.id,confirm:true}));await loadFolder(destination);}
    catch(error){notice(error.message);}
  }
};
function downloadText(text,name,type='text/plain') {
  const url=URL.createObjectURL(new Blob([text],{type}));const link=document.createElement('a');link.href=url;link.download=name;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
}
async function authorizedText(path) {
  const response=await fetch('/api/'+path,{headers:{Authorization:'Bearer '+state.token},cache:'no-store',signal:AbortSignal.timeout(15000)});
  if(!response.ok){let error;try{error=await response.json()}catch{}throw new Error(error?.error||'Recovery could not provide this file.');}return response.text();
}
$('certificateDownload').onclick=async()=>{try{downloadText(await authorizedText('certificate'),'AERA-device.crt','application/x-pem-file')}catch(error){notice(error.message)}};
function renderInstallerDetails(presentation){
  let panel=$('installerDetails');
  if(!panel){panel=document.createElement('div');panel.id='installerDetails';panel.className='installer-details';$('statusTitle').closest('.status-head').before(panel);}
  panel.hidden=!presentation||!Object.keys(presentation).length;panel.replaceChildren();if(panel.hidden)return;
  const title=document.createElement('h3');title.textContent=presentation.package||'Package installation';panel.append(title);
  const rows=[['Device',presentation.device],['Author',presentation.author],
    ['Stage',presentation.stage_count?`${presentation.stage} / ${presentation.stage_count}`:'']];
  const details=document.createElement('dl');details.className='review';
  for(const [name,value] of rows){if(!value)continue;const row=document.createElement('div'),label=document.createElement('dt'),text=document.createElement('dd');label.textContent=name;text.textContent=value;row.append(label,text);details.append(row);}
  panel.append(details);
  for(const value of [presentation.stage_title,presentation.stage_detail,
      presentation.rebooting?`Rebooting to ${presentation.reboot_target||'Android'} in ${presentation.reboot_seconds}s`:'']){
    if(value){const text=document.createElement('p');text.textContent=value;panel.append(text);}
  }
}
function renderPackageInfo(panel,info){
  panel.hidden=!info||!Object.keys(info).length;
  const signature=JSON.stringify(info||{});if(panel.dataset.signature===signature)return;
  panel.dataset.signature=signature;panel.replaceChildren();if(panel.hidden)return;
  if(info.error){const error=document.createElement('div');error.className='notice error';error.textContent=info.error;panel.append(error);return;}
  if(!info.is_payload){const text=document.createElement('p');text.textContent=info.summary;panel.append(text);return;}
  const title=document.createElement('h3');title.textContent=info.build||'Android update';panel.append(title);
  const rows=(parent,values)=>{
    const list=document.createElement('dl');list.className='review';
    for(const [name,value] of values){if(value===undefined||value===null||value==='')continue;const row=document.createElement('div'),label=document.createElement('dt'),text=document.createElement('dd');label.textContent=name;text.textContent=String(value);row.append(label,text);list.append(row)}
    parent.append(list);
  };
  rows(panel,[['Device',info.device],['Android',info.android],['Security patch',info.security_patch],
    ['Update type',info.incremental?'Incremental OTA':'Full OTA'],['Payload size',size(info.payload_bytes)],
    ['Package ARB',info.arb_available?info.arb_index:'Unavailable']]);
  if(['upgrade','downgrade'].includes(info.arb_decision)){
    const warning=document.createElement('div');warning.className='notice'+(info.arb_decision==='downgrade'?' error':'');
    warning.textContent=info.arb_decision==='downgrade'?'Installation blocked: this package lowers the firmware ARB index and can brick the device.':
      'This update raises the firmware ARB index. It may permanently prevent returning to older firmware.';
    panel.append(warning);
  }
  const details=document.createElement('details'),summary=document.createElement('summary');summary.textContent='Full information';details.append(summary);
  rows(details,[[info.name_from_filename?'Package name':'Version',info.build],['Product',info.device],['Device codename',info.codename],
    ['Build ID',info.build_id],['System fingerprint',info.fingerprint],['Partition coverage',info.partial?'Partial update':'Full update'],
    ['Expanded images',size(info.expanded_bytes)],['Format revision',info.format],['Dynamic partitions',info.dynamic_partitions?'Yes':'No'],
    ['Snapshots',info.snapshots?'Requested by package':'Not requested'],['Virtual A/B compression',info.compression?'Requested by package':'Not requested'],
    ['Operations',info.operations],['Operation types',info.operation_types],['ARB source',info.arb_detail],
    ['Slot A ARB',info.slot_a_arb],['Slot B ARB',info.slot_b_arb]]);
  const heading=document.createElement('h3');heading.textContent='Partitions';details.append(heading);
  rows(details,(info.partitions||[]).map(partition=>[partition.name,size(partition.bytes)]));panel.append(details);
}
$('recoveryLog').onclick=async()=>{try{downloadText(await authorizedText('log'),'AERA-recovery.log')}catch(error){notice(error.message)}};
for(const [id,path,name] of [['logcatLog','log/logcat','AERA-logcat.txt'],['kernelLog','log/kernel','AERA-kernel.log']]) $(id).onclick=async()=>{
  $(id).disabled=true;const text=$(id).textContent;$(id).textContent='Collecting...';
  try{downloadText(await authorizedText(path),name)}catch(error){notice(error.message)}
  finally{$(id).textContent=text;$(id).disabled=false;}
};
$('deviceRefresh').onclick=()=>refresh().catch(error=>notice(error.message));
showTab('install');if(state.info)updateInfo(state.info);controls();
