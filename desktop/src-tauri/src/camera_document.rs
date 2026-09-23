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
    let path=fs::canonicalize(path).map_err(|e|e.to_string())?;
    if path.extension().and_then(|x|x.to_str()).map(|x|x.eq_ignore_ascii_case(kind))!=Some(true){return Err("Camera document extension does not match editor".into());}
    let old=read(&path)?;
    let result=match action {
        "read"=>old,
        "save"=>{
            if revision(&old)!=baseline{return Err("Camera file changed on disk; reload and reconcile your draft".into());}
            if text.len()>LIMIT{return Err("Camera document exceeds 4 MiB".into());}
            if kind=="json" && !serde_json::from_str::<serde_json::Value>(text).map_err(|e|e.to_string())?.is_object(){return Err("MindVision JSON must be an object".into());}
            let temporary=path.with_extension(format!("{}.{}.tmp",kind,std::process::id()));
            let mut f=fs::OpenOptions::new().write(true).create_new(true).open(&temporary).map_err(|e|e.to_string())?;
            let save=(||{f.write_all(text.as_bytes())?;f.sync_all()?;fs::set_permissions(&temporary,fs::metadata(&path)?.permissions())?;drop(f);if revision(&read(&path).map_err(std::io::Error::other)?)!=baseline{return Err(std::io::Error::other("Camera file changed during save"));}fs::rename(&temporary,&path)})();
            if let Err(error)=save{let _=fs::remove_file(&temporary);return Err(error.to_string());}
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
        fs::remove_dir_all(dir).unwrap();
    }
}
