//! The status line kept at the bottom of stderr (arc42 §8.10): log events
//! print above it, and it is redrawn below them. Checked over an in-memory
//! terminal, the cursor moves written as `<up 2>` and `<clear>`.

use std::io::Write;

use umbra::Footer;

/// What the terminal received: escapes as `<up 2>` and `<clear>`.
fn received(out: &[u8]) -> String {
    String::from_utf8_lossy(out)
        .replace("\x1b[2A", "<up 2>")
        .replace("\r\x1b[J", "<clear>")
}

#[test]
fn a_status_update_replaces_the_previous_one() {
    let mut out = Vec::new();
    let footer = Footer::new(&mut out);
    let mut status = footer.redrawn();
    write!(status, "Pass 1/2\nLocal Worker\n").unwrap();
    status.flush().unwrap();
    write!(status, "Pass 2/2\nLocal Worker\n").unwrap();
    status.flush().unwrap();
    drop(status);
    drop(footer);
    assert_eq!(
        received(&out),
        "Pass 1/2\nLocal Worker\n<up 2><clear>Pass 2/2\nLocal Worker\n"
    );
}

#[test]
fn a_log_event_prints_above_the_status_which_is_redrawn_below_it() {
    let mut out = Vec::new();
    let footer = Footer::new(&mut out);
    let mut status = footer.redrawn();
    write!(status, "Pass 1/2\nLocal Worker\n").unwrap();
    status.flush().unwrap();
    (&footer).write_all(b"INFO an event\n").unwrap();
    drop(status);
    drop(footer);
    assert_eq!(
        received(&out),
        "Pass 1/2\nLocal Worker\n<up 2><clear>INFO an event\nPass 1/2\nLocal Worker\n"
    );
}

#[test]
fn once_released_the_status_stays_and_log_events_follow_it() {
    let mut out = Vec::new();
    let footer = Footer::new(&mut out);
    let mut status = footer.redrawn();
    write!(status, "Pass 2/2\nLocal Worker\n").unwrap();
    status.flush().unwrap();
    drop(status);
    (&footer).write_all(b"INFO finished\n").unwrap();
    drop(footer);
    assert_eq!(received(&out), "Pass 2/2\nLocal Worker\nINFO finished\n");
}
