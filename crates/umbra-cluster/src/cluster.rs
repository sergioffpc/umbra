//! The Cluster Name (ADR 0021): every Coordinator and Worker belongs to one
//! Cluster, and works only with the others of the same name.

use std::fmt::{self, Display};
use std::str::FromStr;

use serde::{Deserialize, Serialize};

use crate::error::Error;

/// The longest DNS label.
const MAX_LEN: usize = 63;

/// The name of a Cluster: a DNS label, lowercase letters, digits and `-`,
/// neither first nor last, at most 63 characters; `default` unless set.
/// It keeps honest Clusters apart on one LAN: a label, not a secret.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct ClusterName(String);

impl Default for ClusterName {
    fn default() -> Self {
        Self("default".to_owned())
    }
}

impl Display for ClusterName {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl FromStr for ClusterName {
    type Err = Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let allowed = |c: char| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-';
        let is_label = (1..=MAX_LEN).contains(&s.len())
            && s.chars().all(allowed)
            && !s.starts_with('-')
            && !s.ends_with('-');
        if is_label {
            Ok(Self(s.to_owned()))
        } else {
            Err(Error::ClusterName(s.to_owned()))
        }
    }
}
