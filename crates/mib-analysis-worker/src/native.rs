#[cxx::bridge(namespace = "backend::analysis")]
pub mod ffi {
    unsafe extern "C++" {
        include!("backend/analysis/WorkerComposition.h");
        #[rust_name = "scalar"]
        fn scalarPredicate(
            known: bool,
            value: f64,
            has_minimum: bool,
            minimum: f64,
            has_maximum: bool,
            maximum: f64,
        ) -> Result<u8>;
        #[rust_name = "and"]
        fn andPredicate(left: u8, right: u8) -> Result<u8>;
    }
}
