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
    if !["js","json","pz7035"].contains(&kind){return Err("Unsupported camera document kind".into());}
    // The PZ7035 camera document is a .json file; the backend validates its content when it is checked and applied.
    let extension=if kind=="pz7035"{"json"}else{kind};
    if action=="default" {
        let text=match kind {"js"=>include_str!("../../../resources/defaults/egrabberConfig.js"),"pz7035"=>include_str!("../../../resources/defaults/pz7035Camera.json"),_=>include_str!("../../../resources/defaults/mindvisionConfig.json")};
        return Ok(CameraDocument{path:String::new(),text:text.into(),revision:String::new()});
    }
    let requested=Path::new(path);
    let creating=action=="create";
    let path=if creating {
        let parent=fs::canonicalize(requested.parent().ok_or("Missing parent folder")?).map_err(|e|e.to_string())?;
        parent.join(requested.file_name().ok_or("Missing filename")?)
    } else {fs::canonicalize(path).map_err(|e|e.to_string())?};
    if path.extension().and_then(|x|x.to_str()).map(|x|x.eq_ignore_ascii_case(extension))!=Some(true){return Err("Camera document extension does not match editor".into());}
    let old=if creating{String::new()}else{read(&path)?};
    let result=match action {
        "read"=>old,
        "save"|"create"=>{
            if !creating && revision(&old)!=baseline{return Err("Camera file changed on disk; reload and reconcile your draft".into());}
            if text.len()>LIMIT{return Err("Camera document exceeds 4 MiB".into());}
            if kind!="js" && !serde_json::from_str::<serde_json::Value>(text).map_err(|e|e.to_string())?.is_object(){return Err("Camera JSON must be an object".into());}
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
pub fn camera_document(action:String,path:String,kind:String,baseline:String,text:String)->Result<CameraDocument,String>{transact(&action,&path,&kind,&baseline,&text)}

/// The instrument's camera documents directory under the data directory.
pub fn pz7035_directory(data_dir:&Path)->std::path::PathBuf{data_dir.join("camera")}

/// Resolve `requested` (relative to, or inside, `<data>/camera/`) for the PZ7035 camera document: a plain file name directly in that directory, no `..`, no
/// symbolic link on the path or as the file. `creating` makes the directory. The save itself keeps the revision check and the atomic rename.
pub fn confine_pz7035(data_dir:&Path,requested:&str,creating:bool)->Result<String,String>{
    let base=pz7035_directory(data_dir);
    if creating{fs::create_dir_all(&base).map_err(|e|e.to_string())?;}
    let base=fs::canonicalize(&base).map_err(|_|"The camera document directory does not exist yet: save a document with Save As first".to_string())?;
    let requested=Path::new(requested);
    let joined=if requested.is_absolute(){requested.to_path_buf()}else{base.join(requested)};
    if joined.components().any(|c|matches!(c,std::path::Component::ParentDir)){return Err("Camera documents live directly in the instrument's camera folder".into());}
    let name=joined.file_name().ok_or("Missing filename")?.to_owned();
    let parent=fs::canonicalize(joined.parent().ok_or("Missing parent folder")?).map_err(|e|e.to_string())?;
    if parent!=base{return Err(format!("Camera documents live directly in {}",base.display()));}
    let file=parent.join(name);
    if let Ok(meta)=fs::symlink_metadata(&file){if meta.file_type().is_symlink(){return Err("A camera document may not be a symbolic link".into());}}
    Ok(file.to_string_lossy().into_owned())
}

/// The remote entry (the server): only the PZ7035 camera document exists there, confined to `<data>/camera/`. The `js`/`json` kinds take any path and
/// belong to the desktop (Tauri) command, never to the server (it runs with the instrument's privileges), so they are refused. A symlinked `<data>/camera`
/// directory itself is followed (an operator's own link); files inside it may not be links.
pub fn camera_document_confined(data_dir:&Path,action:String,path:String,kind:String,baseline:String,text:String)->Result<CameraDocument,String>{
    if kind!="pz7035"{return Err("Only the PZ7035 camera document is available through the server".into());}
    let path=if action!="default"{confine_pz7035(data_dir,&path,action=="create"||action=="save")?}else{path};
    transact(&action,&path,&kind,&baseline,&text)
}
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
    fn the_remote_pz7035_document_is_confined_to_the_camera_folder() {
        let data=tempfile::tempdir().unwrap();let outside=tempfile::tempdir().unwrap();
        let secret=outside.path().join("secret.json");fs::write(&secret,"{\"secret\":true}").unwrap();
        let (c,r)=("create".to_string(),"read".to_string());
        // The folder is made by the first create; a read before that says so.
        assert!(camera_document_confined(data.path(),r.clone(),"camera.json".into(),"pz7035".into(),String::new(),String::new()).is_err());
        let made=camera_document_confined(data.path(),c.clone(),"camera.json".into(),"pz7035".into(),String::new(),"{\"run\":{}}".into()).unwrap();
        assert!(made.path.ends_with("camera/camera.json"),"{}",made.path);
        // A relative name and the absolute path both reach it.
        assert_eq!(camera_document_confined(data.path(),r.clone(),"camera.json".into(),"pz7035".into(),String::new(),String::new()).unwrap().text,"{\"run\":{}}");
        assert!(camera_document_confined(data.path(),r.clone(),made.path.clone(),"pz7035".into(),String::new(),String::new()).is_ok());
        // Everything else is refused: other folders, parents, absolute paths elsewhere, subfolders, symbolic links (file and folder).
        for bad in [secret.to_str().unwrap().to_string(),"../secret.json".into(),"../config/aravis-camera.json".into(),"sub/camera.json".into(),"/etc/passwd".into()] {
            assert!(camera_document_confined(data.path(),r.clone(),bad.clone(),"pz7035".into(),String::new(),String::new()).is_err(),"{bad}");
            assert!(camera_document_confined(data.path(),c.clone(),bad.clone(),"pz7035".into(),String::new(),"{}".into()).is_err(),"{bad}");
        }
        let camera=data.path().join("camera");
        std::os::unix::fs::symlink(&secret,camera.join("link.json")).unwrap();
        assert!(camera_document_confined(data.path(),r.clone(),"link.json".into(),"pz7035".into(),String::new(),String::new()).is_err());
        assert!(camera_document_confined(data.path(),"save".into(),"link.json".into(),"pz7035".into(),String::new(),"{}".into()).is_err());
        assert_eq!(fs::read_to_string(&secret).unwrap(),"{\"secret\":true}","the file behind the link is untouched");
        // The desktop kinds take any path, so the server refuses them outright (read, create, save and default).
        for kind in ["json","js"] {
            for action in ["read","create","save","default"] {
                assert!(camera_document_confined(data.path(),action.into(),secret.to_str().unwrap().into(),kind.into(),String::new(),"{}".into()).is_err(),"{kind} {action}");
            }
        }
        assert!(!outside.path().join("new.json").exists());
        // The default needs no path; the revision check and the .json extension still apply.
        assert!(camera_document_confined(data.path(),"default".into(),String::new(),"pz7035".into(),String::new(),String::new()).is_ok());
        let again=camera_document_confined(data.path(),r.clone(),"camera.json".into(),"pz7035".into(),String::new(),String::new()).unwrap();
        assert!(camera_document_confined(data.path(),"save".into(),"camera.json".into(),"pz7035".into(),"stale".into(),"{}".into()).is_err());
        assert!(camera_document_confined(data.path(),"save".into(),"camera.json".into(),"pz7035".into(),again.revision,"{\"run\":{\"frame_rate_hz\":1000}}".into()).is_ok());
        assert!(camera_document_confined(data.path(),c,"camera.txt".into(),"pz7035".into(),String::new(),"{}".into()).is_err());
    }
    #[test]
    fn pz7035_documents_are_json_files_with_a_bundled_default() {
        let dir=tempfile::tempdir().unwrap();
        let defaults=transact("default","","pz7035","","").unwrap();
        assert!(defaults.path.is_empty());
        let parsed:serde_json::Value=serde_json::from_str(&defaults.text).unwrap();
        assert_eq!(parsed["schema"],"pz7035-camera/1");assert_eq!(parsed["run"]["frame_rate_hz"],5000);assert_eq!(parsed["run"]["exposure_us"],"auto");
        let path=dir.path().join("camera.json");let target=path.to_str().unwrap();
        transact("create",target,"pz7035","",&defaults.text).unwrap();
        let read=transact("read",target,"pz7035","","").unwrap();assert_eq!(read.text,defaults.text);
        transact("save",target,"pz7035",&read.revision,"{\"run\":{\"frame_rate_hz\":1000}}").unwrap();
        assert!(transact("save",target,"pz7035",&read.revision,"{}").is_err(), "a stale revision is refused");
        let now=transact("read",target,"pz7035","","").unwrap();
        assert!(transact("save",target,"pz7035",&now.revision,"[]").is_err(), "a document must be a JSON object");
        assert!(transact("read",dir.path().join("camera.js").to_str().unwrap(),"pz7035","","").is_err());
        let wrong=dir.path().join("camera.txt");std::fs::write(&wrong,"{}").unwrap();
        assert!(transact("read",wrong.to_str().unwrap(),"pz7035","","").is_err(), "the extension must be .json");
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
