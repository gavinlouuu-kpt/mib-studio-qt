//! Camera configuration files: bounded reads and revision-checked atomic saves.
use serde::Serialize;
use sha2::{Digest,Sha256};
use std::{fs,io::Write,path::Path,sync::Mutex};
static WRITER:Mutex<()>=Mutex::new(());
const LIMIT:usize=4*1024*1024;
#[derive(Serialize)]
pub struct CameraDocument {path:String,text:String,revision:String}
fn read(path:&Path)->Result<String,String>{
    if !fs::metadata(path).map_err(|e|e.to_string())?.is_file(){return Err("Select a regular camera configuration file".into());}
    use std::io::Read;
    let mut bytes=Vec::new();fs::File::open(path).map_err(|e|e.to_string())?.take((LIMIT+1) as u64).read_to_end(&mut bytes).map_err(|e|e.to_string())?;
    if bytes.len()>LIMIT{return Err("Camera document exceeds 4 MiB".into());}
    String::from_utf8(bytes).map_err(|e|e.to_string())
}
fn revision(text:&str)->String{hex::encode(Sha256::digest(text.as_bytes()))}
fn transact(action:&str,path:&str,kind:&str,baseline:&str,text:&str)->Result<CameraDocument,String>{
    let _lock=WRITER.lock().map_err(|e|e.to_string())?;
    if !["js","json"].contains(&kind){return Err("Unsupported camera document kind".into());}
    if action=="default" {
        let text=if kind=="js"{include_str!("../../../resources/defaults/egrabberConfig.js")}else{include_str!("../../../resources/defaults/mindvisionConfig.json")};
        return Ok(CameraDocument{path:String::new(),text:text.into(),revision:String::new()});
    }
    let requested=Path::new(path);
    let creating=action=="create";
    let path=if creating {
        let parent=fs::canonicalize(requested.parent().ok_or("Missing parent folder")?).map_err(|e|e.to_string())?;
        parent.join(requested.file_name().ok_or("Missing filename")?)
    } else {fs::canonicalize(path).map_err(|e|e.to_string())?};
    if path.extension().and_then(|x|x.to_str()).map(|x|x.eq_ignore_ascii_case(kind))!=Some(true){return Err("Camera document extension does not match editor".into());}
    let old=if creating{String::new()}else{read(&path)?};
    let result=match action {
        "read"=>old,
        "save"|"create"=>{
            if !creating && revision(&old)!=baseline{return Err("Camera file changed on disk; reload and reconcile your draft".into());}
            if text.len()>LIMIT{return Err("Camera document exceeds 4 MiB".into());}
            if kind=="json" && !serde_json::from_str::<serde_json::Value>(text).map_err(|e|e.to_string())?.is_object(){return Err("MindVision JSON must be an object".into());}
            // Staging and destination share a filesystem. RAII removes the staging
            // file on write/sync, revision-check or publication failure.
            let mut temporary=tempfile::NamedTempFile::new_in(path.parent().ok_or("Missing parent folder")?).map_err(|e|e.to_string())?;
            temporary.write_all(text.as_bytes()).map_err(|e|e.to_string())?;
            temporary.as_file().sync_all().map_err(|e|e.to_string())?;
            if creating {
                // Native no-replace rename where supported (including Windows
                // FAT/exFAT); never emulate this using an existence-check+rename.
                temporary.persist_noclobber(&path).map_err(|e|e.to_string())?;
            } else {
                temporary.as_file().set_permissions(fs::metadata(&path).map_err(|e|e.to_string())?.permissions()).map_err(|e|e.to_string())?;
                if revision(&read(&path)?)!=baseline{return Err("Camera file changed during save".into());}
                temporary.persist(&path).map_err(|e|e.to_string())?;
            }
            text.to_owned()
        },
        _=>return Err("Unsupported camera document action".into())
    };
    Ok(CameraDocument{path:path.to_string_lossy().into_owned(),revision:revision(&result),text:result})
}
#[tauri::command]
pub fn camera_document(action:String,path:String,kind:String,baseline:String,text:String)->Result<CameraDocument,String>{transact(&action,&path,&kind,&baseline,&text)}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn checked_save_preserves_conflicting_file_and_rejects_bad_json(){
        let dir=std::env::temp_dir().join(format!("mib-camera-document-{}",std::process::id()));fs::create_dir_all(&dir).unwrap();let path=dir.join("camera.json");fs::write(&path,"{}").unwrap();let p=path.to_str().unwrap();
        let d=transact("read",p,"json","","").unwrap();
        assert!(transact("save",p,"json",&d.revision,"[]").is_err());
        fs::write(&path,"{\"changed\":true}").unwrap();assert!(transact("save",p,"json",&d.revision,"{}").is_err());
        let d=transact("read",p,"json","","").unwrap();transact("save",p,"json",&d.revision,"{\"saved\":true}").unwrap();assert_eq!(fs::read_to_string(&path).unwrap(),"{\"saved\":true}");
        let copied=dir.join("copy.json");let target=copied.to_str().unwrap();
        transact("create",target,"json","","{\"copy\":true}").unwrap();
        assert!(transact("create",target,"json","","{}").is_err());
        assert_eq!(fs::read_to_string(&copied).unwrap(),"{\"copy\":true}");
        let defaults=transact("default","","json","","").unwrap();assert!(defaults.path.is_empty());assert!(serde_json::from_str::<serde_json::Value>(&defaults.text).unwrap().is_object());
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn save_as_roundtrips_and_failed_publication_removes_staging() {
        let dir=tempfile::tempdir().unwrap();
        let path=dir.path().join("camera.json");
        let text="{\"script\":\"unicode µ and precise 1.2345\"}";
        let result=transact("create",path.to_str().unwrap(),"json","",text).unwrap();
        assert_eq!(result.text,text);assert_eq!(result.revision,revision(text));
        assert_eq!(fs::read_to_string(&path).unwrap(),text);
        assert!(transact("create",path.to_str().unwrap(),"json","","{}").is_err());
        assert_eq!(fs::read_to_string(&path).unwrap(),text);
        let directory=dir.path().join("directory.json");fs::create_dir(&directory).unwrap();
        assert!(transact("create",directory.to_str().unwrap(),"json","","{}").is_err());
        assert!(directory.is_dir());
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(),2,"failed persistence cleans every temporary file");
    }
    #[test]
    fn save_as_is_bounded_and_not_blocked_by_legacy_staging_name() {
        let dir=tempfile::tempdir().unwrap();let path=dir.path().join("camera.js");
        let old=path.with_extension(format!("js.{}.tmp",std::process::id()));
        fs::write(&old,"stale unrelated draft").unwrap();
        assert!(transact("create",path.to_str().unwrap(),"js","",&"x".repeat(LIMIT+1)).is_err());
        assert!(!path.exists());assert_eq!(fs::read_dir(dir.path()).unwrap().count(),1);
        transact("create",path.to_str().unwrap(),"js","","// new script").unwrap();
        assert_eq!(fs::read_to_string(old).unwrap(),"stale unrelated draft");
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(),2);
    }
    #[test]
    fn simultaneous_save_as_has_exactly_one_winner_and_no_partial_file() {
        // A mutex/barrier regression must fail finitely rather than hang CI.
        let (_watchdog_stop, watchdog_wait)=std::sync::mpsc::channel::<()>();
        std::thread::spawn(move||{
            if matches!(watchdog_wait.recv_timeout(std::time::Duration::from_secs(20)),Err(std::sync::mpsc::RecvTimeoutError::Timeout)) {
                eprintln!("Camera document concurrent-create watchdog expired");
                std::process::abort();
            }
        });
        let dir=tempfile::tempdir().unwrap();let path=dir.path().join("camera.json");
        let barrier=std::sync::Arc::new(std::sync::Barrier::new(2));
        let handles:Vec<_>=["{\"writer\":1}","{\"writer\":2}"].into_iter().map(|text|{
            let barrier=barrier.clone();let path=path.clone();
            std::thread::spawn(move||{barrier.wait();transact("create",path.to_str().unwrap(),"json","",text).is_ok()})
        }).collect();
        assert_eq!(handles.into_iter().map(|handle|usize::from(handle.join().unwrap())).sum::<usize>(),1);
        let saved=fs::read_to_string(path).unwrap();
        assert!(saved=="{\"writer\":1}" || saved=="{\"writer\":2}");
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(),1);
    }

    #[cfg(unix)]
    #[test]
    fn replacement_preserves_existing_permissions() {
        use std::os::unix::fs::PermissionsExt;
        let dir=tempfile::tempdir().unwrap();let path=dir.path().join("camera.js");
        fs::write(&path,"old").unwrap();
        fs::set_permissions(&path,fs::Permissions::from_mode(0o640)).unwrap();
        transact("save",path.to_str().unwrap(),"js",&revision("old"),"new").unwrap();
        assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o777,0o640);
        assert_eq!(fs::read_to_string(path).unwrap(),"new");
    }

}
