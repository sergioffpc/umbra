//! The committed JSON Schema of the Render Job Report is the one its Rust
//! types generate (arc42 §8.10).

use std::env;
use std::fs;
use std::path::Path;

use umbra_cluster::RenderJobReport;

/// Where the schema is committed.
const SCHEMA: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/render-job-report.schema.json");

#[test]
fn the_committed_schema_is_the_generated_one() {
    let generated = serde_json::to_string_pretty(&RenderJobReport::json_schema()).unwrap() + "\n";
    if env::var_os("UMBRA_BLESS").is_some() {
        fs::write(SCHEMA, &generated).unwrap();
    }
    let committed = fs::read_to_string(Path::new(SCHEMA)).unwrap_or_default();
    assert!(
        committed == generated,
        "{SCHEMA} is stale: rerun this test with UMBRA_BLESS=1 to regenerate it"
    );
}
