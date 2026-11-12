//! The Render Job Report (arc42 §8.10): Umbra's own JSON, versioned
//! `major.minor`. So far its outcome, Film hash and Reproduction Record;
//! the rest of §8.10 arrives with
//! [#86](https://github.com/sergioffpc/umbra/issues/86).

use std::io::{self, Write};
use std::path::Path;

use serde::{Serialize, Serializer};
use umbra_render::{FilmHash, ReproductionRecord, write_atomically};

/// How a Render Job ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Outcome {
    /// Every Pass was rendered and merged; the image exists.
    Finished,
}

/// What a Render Job leaves behind besides its image, written next to it
/// as `<image>.report.json`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct RenderJobReport {
    schema_version: &'static str,
    outcome: Outcome,
    #[serde(serialize_with = "hex")]
    film_hash: Option<FilmHash>,
    reproduction_record: ReproductionRecord,
}

impl RenderJobReport {
    /// The schema version: the major part changes with the meaning of a
    /// comparable field, the minor part with any other change.
    pub const SCHEMA_VERSION: &str = "0.1";

    /// The report of a finished Render Job whose Film hashes to
    /// `film_hash`, made as `reproduction_record` says.
    #[must_use]
    pub fn finished(film_hash: FilmHash, reproduction_record: ReproductionRecord) -> Self {
        Self {
            schema_version: Self::SCHEMA_VERSION,
            outcome: Outcome::Finished,
            film_hash: Some(film_hash),
            reproduction_record,
        }
    }

    /// How the Render Job ended.
    #[must_use]
    pub fn outcome(&self) -> Outcome {
        self.outcome
    }

    /// The Film hash, when the Render Job finished.
    #[must_use]
    pub fn film_hash(&self) -> Option<FilmHash> {
        self.film_hash
    }

    /// Writes the report to `path` as indented JSON, its fields in a fixed
    /// order, replacing any file there.
    ///
    /// # Errors
    ///
    /// Any I/O error; `path` is then left as it was.
    pub fn write(&self, path: &Path) -> io::Result<()> {
        write_atomically(path, |file| {
            serde_json::to_writer_pretty(&mut *file, self)?;
            writeln!(file)
        })
    }
}

/// Serialises a Film hash as its hexadecimal digits, or `null`.
#[expect(
    clippy::ref_option,
    reason = "`serialize_with` passes a reference to the field"
)]
fn hex<S: Serializer>(hash: &Option<FilmHash>, serializer: S) -> Result<S::Ok, S::Error> {
    match hash {
        Some(hash) => serializer.collect_str(hash),
        None => serializer.serialize_none(),
    }
}
