//! User-operated Tauri installers, using the existing HTTPS manifest/SHA256 trust contract.
use crate::{updater::{parse_manifest, UpdateManifest}, AppState};
use serde::{Deserialize, Serialize};
use sha2::{Digest,Sha256};
use std::{fs::{self,File,OpenOptions},io::{Read,Write},path::{Path,PathBuf},sync::{Mutex,OnceLock},time::{Instant,Duration,SystemTime,UNIX_EPOCH}};
use tauri::{Manager,AppHandle};
use tauri_plugin_opener::OpenerExt;
const LIMIT:u64=4*1024*1024*1024;
const TTL:Duration=Duration::from_secs(15*60);
#[derive(Clone,Debug,PartialEq,Eq,Deserialize)]
struct Release {#[serde(flatten)] manifest:UpdateManifest,artifact_family:String,os:String,arch:String,installer_size_bytes:u64}
#[derive(Clone)]
struct Ticket {token:String,release:Release,feed:String,checked:Instant,staged:Option<PathBuf>}
static TICKET:OnceLock<Mutex<Option<Ticket>>>=OnceLock::new();
fn tickets()->&'static Mutex<Option<Ticket>>{TICKET.get_or_init(||Mutex::new(None))}
fn https(text:&str)->Result<tauri::Url,String>{let url=tauri::Url::parse(text).map_err(|e|e.to_string())?;if url.scheme()!="https"||url.host_str().is_none()||!url.username().is_empty()||url.password().is_some()||url.fragment().is_some(){return Err("Update URLs require HTTPS without credentials or fragments".into());}Ok(url)}
fn validate(body:&str,current:&str,os:&str,arch:&str)->Result<Release,String>{
 let manifest=parse_manifest(body).map_err(|e|e.to_string())?;
 let release:Release=serde_json::from_str(body).map_err(|e|e.to_string())?;
 if release.artifact_family!="tauri"{return Err("Only explicitly identified Tauri installers are accepted; Qt artifacts are rejected".into());}
 if release.os!=os||release.arch!=arch{return Err("Installer platform does not match this application".into());}
 let incoming=semver::Version::parse(&manifest.version).map_err(|_|"Invalid release semantic version")?;
 let installed=semver::Version::parse(current).map_err(|_|"Cannot verify installed application version")?;
 if incoming<=installed{return Err("Release is not newer than the installed application".into());}
 if release.installer_size_bytes==0||release.installer_size_bytes>LIMIT{return Err("Installer size must be positive and at most 4 GiB".into());}
 let url=https(&manifest.url)?;
 let extension=Path::new(url.path()).extension().and_then(|x|x.to_str()).unwrap_or("").to_ascii_lowercase();
 if !match os{"windows"=>["exe","msi"].contains(&extension.as_str()),"linux"=>["deb","rpm"].contains(&extension.as_str()),_=>false}{return Err("Unsupported installer format for this platform (Windows exe/msi; Linux deb/rpm)".into());}
 Ok(release)
}
fn fresh(ticket:&Ticket)->Result<(),String>{if ticket.checked.elapsed()>TTL{return Err("Update check expired; check the release again".into());}Ok(())}
fn same_release(ticket:&Ticket,latest:&Release)->Result<(),String>{fresh(ticket)?;if &ticket.release!=latest{return Err("Release manifest changed; check and verify the installer again".into());}Ok(())}
fn fetch(feed:&str)->Result<String,String>{
 https(feed)?;
 let value:serde_json::Value=serde_json::from_str(&mib_bridge::ffi::profile_fetch_url(feed)).map_err(|e|e.to_string())?;
 if value["ok"]!=true{return Err(value["error"].as_str().unwrap_or("Update manifest fetch failed").into());}
 Ok(value["body"].as_str().ok_or("Missing update manifest body")?.to_owned())
}
fn require_idle(app:&AppHandle)->Result<(),String>{
 let state=app.state::<AppState>();let mut bridge=state.bridge.lock().map_err(|e|e.to_string())?;
 require_idle_locked(&mut bridge)
}
fn require_idle_locked(bridge:&mut cxx::UniquePtr<mib_bridge::ffi::BackendBridge>)->Result<(),String>{
 let experiment=bridge.pin_mut().fetch_experiment_status();
 if experiment.flushing || experiment.valid_buffered>0 || experiment.invalid_buffered>0 || (experiment.state==4 && !experiment.terminal) {return Err("Experiment finalization is still pending".into());}
 let runtime:serde_json::Value=serde_json::from_str(&bridge.pin_mut().fetch_preview_buffer()).map_err(|e|e.to_string())?;
 let export:serde_json::Value=serde_json::from_str(&bridge.pin_mut().review_export_status_json()).map_err(|e|e.to_string())?;
 let reanalysis:serde_json::Value=serde_json::from_str(&bridge.pin_mut().review_reanalysis_status_json()).map_err(|e|e.to_string())?;
 let calibration:serde_json::Value=serde_json::from_str(&bridge.pin_mut().background_calibration_status()).map_err(|e|e.to_string())?;
 idle(experiment.valid,experiment.state,runtime["capture_running"].as_bool(),runtime["recording"].as_bool(),&[export,reanalysis,calibration])?;
 Ok(())
}
fn idle(valid:bool,state:u32,capture:Option<bool>,recording:Option<bool>,jobs:&[serde_json::Value])->Result<(),String>{
 if !valid||![0,4].contains(&state)||capture!=Some(false)||recording!=Some(false)||jobs.iter().any(|j|!matches!(j["state"].as_str(),Some("idle"|"completed"|"cancelled"|"failed"|"succeeded"|"failed_insufficient"|"failed_timeout"|"failed_processing"))){return Err("Stop capture/recording and finish all experiments, exports, reanalysis and calibration before updating".into());}Ok(())
}
fn copy_verified(source:&Path,destination:&Path,release:&Release)->Result<(),String>{
 let metadata=fs::symlink_metadata(source).map_err(|e|e.to_string())?;
 if !metadata.is_file()||metadata.len()!=release.installer_size_bytes{return Err("Installer must be a regular file with the published size".into());}
 let mut input=File::open(source).map_err(|e|e.to_string())?;
 let mut output=OpenOptions::new().write(true).create_new(true).open(destination).map_err(|e|e.to_string())?;
 let result=(||{let mut hash=Sha256::new();let mut buffer=[0u8;64*1024];let mut size=0u64;
 loop{let n=input.read(&mut buffer).map_err(|e|e.to_string())?;if n==0{break;}size+=n as u64;if size>release.installer_size_bytes||size>LIMIT{return Err("Installer grew while reading".into());}hash.update(&buffer[..n]);output.write_all(&buffer[..n]).map_err(|e|e.to_string())?;}
 if size!=release.installer_size_bytes||hex::encode(hash.finalize())!=release.manifest.sha256.trim().to_ascii_lowercase(){return Err("Installer SHA256/size mismatch; nothing was launched".into());}output.sync_all().map_err(|e|e.to_string())?;Ok(())})();
 drop(output);if result.is_err(){let _=fs::remove_file(destination);}result
}
fn verify_staged(path:&Path,release:&Release)->Result<(),String>{let mut file=File::open(path).map_err(|e|e.to_string())?;let mut hash=Sha256::new();let mut size=0;let mut b=[0u8;64*1024];loop{let n=file.read(&mut b).map_err(|e|e.to_string())?;if n==0{break;}size+=n as u64;if size>release.installer_size_bytes{return Err("Staged installer changed".into());}hash.update(&b[..n]);}if size!=release.installer_size_bytes||hex::encode(hash.finalize())!=release.manifest.sha256.trim().to_ascii_lowercase(){return Err("Staged installer changed".into());}Ok(())}
#[derive(Serialize)]
pub struct CheckedRelease {token:String,version:String,url:String,sha256:String,artifact_family:String,os:String,arch:String,installer_size_bytes:u64}
#[tauri::command]
pub async fn check_tauri_app_update(app:AppHandle,channel:String)->Result<CheckedRelease,String>{
 if channel!="stable"&&channel!="beta"{return Err("Unsupported release channel".into());}
 tauri::async_runtime::spawn_blocking(move||{
 require_idle(&app)?;let current=app.package_info().version.to_string();let os=std::env::consts::OS;let arch=std::env::consts::ARCH;
 let feed=format!("https://updates.yofo.bio/{channel}/tauri/{os}-{arch}/latest.json");
 let release=validate(&fetch(&feed)?,&current,os,arch)?;
 if release.manifest.channel.as_deref()!=Some(channel.as_str()){return Err("Manifest release channel mismatch".into());}
 let token=hex::encode(Sha256::digest(format!("{}:{}:{}",feed,release.manifest.sha256,SystemTime::now().duration_since(UNIX_EPOCH).map_err(|e|e.to_string())?.as_nanos())));
 let result=CheckedRelease{token:token.clone(),version:release.manifest.version.clone(),url:release.manifest.url.clone(),sha256:release.manifest.sha256.clone(),artifact_family:release.artifact_family.clone(),os:release.os.clone(),arch:release.arch.clone(),installer_size_bytes:release.installer_size_bytes};
 let mut state=tickets().lock().map_err(|e|e.to_string())?;if let Some(old)=state.take(){if let Some(path)=old.staged{let _=fs::remove_file(path);}}
 *state=Some(Ticket{token,release,feed,checked:Instant::now(),staged:None});Ok(result)
 }).await.map_err(|e|e.to_string())?
}
#[tauri::command]
pub async fn verify_tauri_app_installer(app:AppHandle,token:String,path:String,ui_idle:bool)->Result<(),String>{
 if !ui_idle{return Err("Save/discard drafts and finish pending UI work before updating".into());}
 tauri::async_runtime::spawn_blocking(move||{
 require_idle(&app)?;let current=app.package_info().version.to_string();let mut state=tickets().lock().map_err(|e|e.to_string())?;let ticket=state.as_mut().filter(|t|t.token==token).ok_or("Update check is no longer current")?;fresh(ticket)?;
 let latest=validate(&fetch(&ticket.feed)?,&current,std::env::consts::OS,std::env::consts::ARCH)?;same_release(ticket,&latest)?;
 let root=app.path().app_cache_dir().map_err(|e|e.to_string())?.join("verified-app-installers");fs::create_dir_all(&root).map_err(|e|e.to_string())?;
 #[cfg(unix)]{use std::os::unix::fs::PermissionsExt;fs::set_permissions(&root,fs::Permissions::from_mode(0o700)).map_err(|e|e.to_string())?;}
 let extension=Path::new(https(&ticket.release.manifest.url)?.path()).extension().and_then(|x|x.to_str()).ok_or("Installer extension unavailable")?.to_owned();
 let destination=root.join(format!("{}.{extension}",ticket.token));
 if let Some(old)=ticket.staged.take(){let _=fs::remove_file(old);}
 copy_verified(Path::new(&path),&destination,&ticket.release)?;if let Err(error)=require_idle(&app){let _=fs::remove_file(&destination);return Err(error);}ticket.staged=Some(destination);Ok(())
 }).await.map_err(|e|e.to_string())?
}
#[tauri::command]
pub async fn launch_tauri_app_installer(app:AppHandle,token:String,ui_idle:bool,confirmed:bool)->Result<(),String>{
 if !ui_idle||!confirmed{return Err("Explicit confirmation with clean idle UI is required".into());}
 tauri::async_runtime::spawn_blocking(move||{
 require_idle(&app)?;let current=app.package_info().version.to_string();let mut state=tickets().lock().map_err(|e|e.to_string())?;let ticket=state.as_ref().filter(|t|t.token==token).ok_or("Update check is no longer current")?;
 let latest=validate(&fetch(&ticket.feed)?,&current,std::env::consts::OS,std::env::consts::ARCH)?;same_release(ticket,&latest)?;
 let path=ticket.staged.as_ref().ok_or("Verify the downloaded installer first")?;verify_staged(path,&ticket.release)?;
 let native=app.state::<AppState>();let mut bridge=native.bridge.lock().map_err(|e|e.to_string())?;require_idle_locked(&mut bridge)?;
 app.opener().open_path(path.to_string_lossy().to_string(),None::<&str>).map_err(|e|e.to_string())?;
 // Never exit on verification/launch errors; no forced exit even after accepted launch.
 *state=None;Ok(())
 }).await.map_err(|e|e.to_string())?
}
#[cfg(test)]
mod tests {
 use super::*;
 fn fixture()->String{serde_json::json!({"version":"9.0.0","installer_url":"https://updates.yofo.bio/app.deb","installer_sha256":hex::encode(Sha256::digest(b"fixture")),"channel":"stable","artifact_family":"tauri","os":"linux","arch":"x86_64","installer_size_bytes":7}).to_string()}
 #[test]fn rejects_wrong_family_platform_version_and_transport(){let base:serde_json::Value=serde_json::from_str(&fixture()).unwrap();assert!(validate(&fixture(),"1.0.0","linux","x86_64").is_ok());for(key,value)in[("artifact_family","qt"),("os","windows"),("arch","aarch64"),("version","0.9.0"),("installer_url","http://updates.yofo.bio/app.deb"),("installer_url","https://attacker@updates.yofo.bio/app.deb")]{let mut invalid=base.clone();invalid[key]=value.into();assert!(validate(&invalid.to_string(),"1.0.0","linux","x86_64").is_err(),"{key}");}}
 #[test]fn installer_version_uses_the_shell_package_not_native_core_compatibility(){
  let installed=semver::Version::parse(env!("CARGO_PKG_VERSION")).unwrap();
  let mut newer=installed.clone();newer.patch+=1;newer.pre=semver::Prerelease::EMPTY;
  let mut release:serde_json::Value=serde_json::from_str(&fixture()).unwrap();release["version"]=newer.to_string().into();
  // The shell package may be 0.1.x while the independent native/core host is 0.2.x.
  // That native compatibility number must not suppress a newer shell installer.
  assert!(validate(&release.to_string(),&installed.to_string(),"linux","x86_64").is_ok());
  let unrelated_native=semver::Version::new(newer.major+1,0,0);
  assert!(validate(&release.to_string(),&unrelated_native.to_string(),"linux","x86_64").is_err());
 }
 #[test]fn stale_and_changed_manifests_fail_closed(){let release=validate(&fixture(),"1.0.0","linux","x86_64").unwrap();let mut ticket=Ticket{token:"fixture".into(),release:release.clone(),feed:"https://updates.yofo.bio".into(),checked:Instant::now(),staged:None};assert!(same_release(&ticket,&release).is_ok());let mut changed=release.clone();changed.manifest.version="9.0.1".into();assert!(same_release(&ticket,&changed).is_err());ticket.checked=Instant::now()-TTL-Duration::from_secs(1);assert!(fresh(&ticket).is_err());}
 #[test]fn active_or_unknown_native_state_blocks_install(){let idle_job=serde_json::json!({"state":"idle"});assert!(idle(true,0,Some(false),Some(false),&[idle_job.clone()]).is_ok());assert!(idle(true,2,Some(false),Some(false),&[idle_job.clone()]).is_err());assert!(idle(true,0,Some(true),Some(false),&[idle_job.clone()]).is_err());assert!(idle(true,0,Some(false),Some(true),&[idle_job.clone()]).is_err());assert!(idle(false,0,Some(false),Some(false),&[idle_job]).is_err());assert!(idle(true,0,Some(false),Some(false),&[serde_json::json!({"state":"running"})]).is_err());assert!(idle(true,0,None,Some(false),&[]).is_err());}
 #[test]fn streaming_copy_detects_tamper_and_staged_mutation_without_launch(){let root=std::env::temp_dir().join(format!("mib-update-fixture-{}-{}",std::process::id(),SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()));fs::create_dir(&root).unwrap();let source=root.join("input.deb");let staged=root.join("verified.deb");let release=validate(&fixture(),"1.0.0","linux","x86_64").unwrap();fs::write(&source,b"fixture").unwrap();copy_verified(&source,&staged,&release).unwrap();verify_staged(&staged,&release).unwrap();fs::write(&staged,b"tamper!").unwrap();assert!(verify_staged(&staged,&release).is_err());fs::remove_file(&staged).unwrap();fs::write(&source,b"tamper!").unwrap();assert!(copy_verified(&source,&staged,&release).is_err());assert!(!staged.exists());fs::remove_dir_all(root).unwrap();}
}
