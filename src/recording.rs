//! Whether a recording still describes the source it was made from.

use crate::json::Json;
use crate::project::Project;
use crate::util;

/// The saved hash is taken by `record`; when it differs, the cases describe code
/// that no longer exists and `diff` passing would mean nothing.
pub fn stale(project: &Project) -> Option<String> {
    let path = project
        .dir
        .join(crate::project::META_DIR)
        .join("recorded-from.json");
    let stored = util::read(&path)
        .ok()
        .and_then(|text| Json::parse(&text).ok())
        .and_then(|doc| {
            doc.get("source_hash")
                .and_then(Json::as_str)
                .map(str::to_string)
        })?;
    let current = util::hash64(&util::read(&project.source_file).unwrap_or_default());
    if stored == current {
        None
    } else {
        Some(format!(
            "{} changed since it was recorded: re-run `cpp-anything record {}` so the port is \
             checked against what the original does now",
            util::short_path(&project.source_file),
            project.dir.display()
        ))
    }
}
