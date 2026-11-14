//! The Cluster Name: a DNS label, `default` unless set (ADR 0021).

use umbra_cluster::ClusterName;

#[test]
fn the_default_cluster_is_named_default() {
    assert_eq!(ClusterName::default().to_string(), "default");
}

#[test]
fn a_dns_label_is_a_cluster_name() {
    for name in ["r630", "render-farm-2", "a", &"x".repeat(63)] {
        let parsed: ClusterName = name.parse().unwrap_or_else(|e| panic!("{name}: {e}"));
        assert_eq!(parsed.to_string(), name);
    }
}

#[test]
fn anything_but_a_dns_label_is_refused_with_the_rule() {
    for name in [
        "",
        "R630",
        "under_score",
        "dot.ted",
        "-lead",
        "trail-",
        "é",
        &"x".repeat(64),
    ] {
        let error = name.parse::<ClusterName>().unwrap_err().to_string();
        assert!(
            error.contains("lowercase letters, digits and `-`, at most 63 characters"),
            "{name:?}: {error}"
        );
    }
}
