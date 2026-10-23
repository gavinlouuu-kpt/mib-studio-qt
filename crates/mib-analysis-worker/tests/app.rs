use mib_analysis_worker::app::{request, Scheduler, Submission};
use serde_json::json;
use std::time::{Duration, Instant};
fn submission() -> (Submission, serde_json::Value) {
    let s = Submission {
        command_id: "app-job".into(),
        plan_id: "plan".into(),
        revision: "1".into(),
        source: json!({"kind":"stored","path":"test.h5"}),
        source_fingerprint: "sha256:test".into(),
        x: "area".into(),
        y: "deformability".into(),
        vertices: vec![[0., 0.], [10., 0.], [10., 1.], [0., 1.]],
    };
    let v = json!({"status":"ok","source":{"fingerprint":"sha256:test","parameters":{"area":"pixel2","deformability":"dimensionless"},
        "rows":[{"key":"a","measurements":{"area":5.,"deformability":0.5}},
        {"key":"b","measurements":{"area":5.,"deformability":0.5}},
        {"key":"c","measurements":{"area":null,"deformability":0.5}}]},
        "evaluation":{"qcFingerprint":"sha256:qc","rows":[{"key":"c","membership":"excluded"},{"key":"a","membership":"selected"},{"key":"b","membership":"unknown"}]}});
    (s, v)
}
#[test]
fn native_source_input_is_keyed_and_fenced() {
    let (s, mut v) = submission();
    let r = request(&s, &v).unwrap();
    let values = &r.scalar_gate.as_ref().unwrap().values;
    assert_eq!(values[0].value, Some(1.));
    assert_eq!(values[1].value, None);
    assert_eq!(values[2].value, Some(0.));
    v["source"]["fingerprint"] = json!("other");
    assert!(request(&s, &v).is_err());
}
#[test]
#[ignore = "requires configured Cytoflow runtime and script"]
fn scheduler_terminal_reopen_cancel_and_shutdown() {
    let dir = tempfile::tempdir().unwrap();
    let (s, v) = submission();
    let id;
    {
        let mut scheduler = Scheduler::open(dir.path()).unwrap();
        let reply = scheduler.submit(request(&s, &v).unwrap()).unwrap();
        id = reply["jobId"].as_str().unwrap().to_string();
        assert!(scheduler.submit(request(&s, &v).unwrap()).is_err()); // one active job
        let deadline = Instant::now() + Duration::from_secs(65);
        loop {
            let result = scheduler.query(&id).unwrap();
            if result["status"] == "terminal" {
                assert!(result["terminal"]["failure"].is_null(), "{result}");
                let rows = &result["terminal"]["result"]["rows"];
                assert_eq!(rows[0]["value"], "true");
                assert_eq!(rows[1]["value"], "unknown");
                assert_eq!(rows[2]["value"], "false");
                break;
            }
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(25));
        }
        assert_eq!(
            scheduler.submit(request(&s, &v).unwrap()).unwrap()["status"],
            "terminal"
        );
    }
    {
        let mut scheduler = Scheduler::open(dir.path()).unwrap();
        assert_eq!(scheduler.query(&id).unwrap()["status"], "terminal");
        let mut r = request(&s, &v).unwrap();
        r.job = "cancel".into();
        let cancelled = scheduler.submit(r).unwrap()["jobId"]
            .as_str()
            .unwrap()
            .to_owned();
        scheduler.cancel(&cancelled).unwrap();
        // Drop must join and persist cancellation; reopen must never rerun.
    }
    let scheduler = Scheduler::open(dir.path()).unwrap();
    let cancelled = scheduler.query(&Scheduler::id("cancel")).unwrap();
    assert_eq!(cancelled["terminal"]["failure"], "Cancelled");
}
