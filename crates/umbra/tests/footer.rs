//! The status line kept at the bottom of stderr (arc42 §8.10): log events
//! print above it, and it is redrawn below them, each line cut to the
//! terminal's width. Checked on `indicatif`'s in-memory terminal, by what
//! its screen shows.

use std::io::Write;

use indicatif::{InMemoryTerm, ProgressDrawTarget};
use umbra::Footer;

/// An in-memory terminal `columns` wide, and a footer on it.
fn terminal(columns: u16) -> (InMemoryTerm, Footer) {
    let term = InMemoryTerm::new(8, columns);
    let footer = Footer::new(ProgressDrawTarget::term_like(Box::new(term.clone())));
    (term, footer)
}

#[test]
fn a_status_update_replaces_the_previous_one() {
    let (term, footer) = terminal(40);
    let mut status = footer.redrawn();
    write!(status, "Pass 1/2\nLocal Worker\n").unwrap();
    status.flush().unwrap();
    write!(status, "Pass 2/2\nLocal Worker\n").unwrap();
    status.flush().unwrap();
    assert_eq!(term.contents(), "Pass 2/2\nLocal Worker");
}

#[test]
fn a_log_event_prints_above_the_status_which_is_redrawn_below_it() {
    let (term, footer) = terminal(40);
    let mut status = footer.redrawn();
    write!(status, "Pass 1/2\nLocal Worker\n").unwrap();
    status.flush().unwrap();
    (&footer).write_all(b"INFO an event\n").unwrap();
    assert_eq!(term.contents(), "INFO an event\nPass 1/2\nLocal Worker");
}

#[test]
fn a_status_line_wider_than_the_terminal_is_cut_to_its_width() {
    let (term, footer) = terminal(20);
    let mut status = footer.redrawn();
    for pass in 1..=3 {
        write!(
            status,
            "Pass {pass}/3 · 30/60 Work Units\nLocal Worker: alive, 16 threads\n"
        )
        .unwrap();
        status.flush().unwrap();
    }
    (&footer).write_all(b"INFO an event\n").unwrap();
    assert_eq!(
        term.contents(),
        "INFO an event\nPass 3/3 · 30/60 Wor\nLocal Worker: alive,"
    );
}

#[test]
fn once_released_the_status_stays_and_log_events_follow_it() {
    let (term, footer) = terminal(40);
    let mut status = footer.redrawn();
    write!(status, "Pass 2/2\nLocal Worker\n").unwrap();
    status.flush().unwrap();
    drop(status);
    (&footer).write_all(b"INFO finished\n").unwrap();
    assert_eq!(term.contents(), "Pass 2/2\nLocal Worker\nINFO finished");
}
