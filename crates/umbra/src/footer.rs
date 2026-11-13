//! The status line's place at the bottom of a terminal (arc42 §8.10): log
//! events print above it, and it is redrawn below them.

use std::io::{self, Write};
use std::sync::{Mutex, MutexGuard, PoisonError};

/// A terminal whose last lines, the footer, are redrawn in place, while
/// other text scrolls above them. Log events are written through `&Footer`;
/// the footer through [`Footer::redrawn`].
#[derive(Debug)]
pub(crate) struct Footer<W> {
    state: Mutex<State<W>>,
}

/// The terminal and what its footer shows.
#[derive(Debug)]
struct State<W> {
    out: W,
    /// The footer's lines on the terminal, each ending in `\n`.
    shown: Vec<u8>,
}

impl<W: Write> Footer<W> {
    /// A footer on `out`, showing nothing yet.
    pub(crate) fn new(out: W) -> Self {
        Self {
            state: Mutex::new(State {
                out,
                shown: Vec::new(),
            }),
        }
    }

    /// The writer of the footer: each flush replaces what the footer shows
    /// with what was written since the last.
    pub(crate) fn redrawn(&self) -> Redrawn<'_, W> {
        Redrawn {
            footer: self,
            pending: Vec::new(),
        }
    }

    /// The terminal, whatever a panicking writer left in it.
    fn lock(&self) -> MutexGuard<'_, State<W>> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }
}

impl<W: Write> State<W> {
    /// Moves to the footer's first line and clears it and every line
    /// below: the cursor is where the footer was.
    fn erase(&mut self) -> io::Result<()> {
        // One more piece than there are newlines.
        let lines = self.shown.split(|&byte| byte == b'\n').count() - 1;
        if lines > 0 {
            write!(self.out, "\x1b[{lines}A\r\x1b[J")?;
        }
        Ok(())
    }
}

/// Log events: each write prints above the footer.
impl<W: Write> Write for &Footer<W> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let mut state = self.lock();
        state.erase()?;
        let State { out, shown } = &mut *state;
        out.write_all(buf)?;
        out.write_all(shown)?;
        drop(state);
        Ok(buf.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        self.lock().out.flush()
    }
}

/// The writer of a [`Footer`]'s lines. Once dropped, the footer's last
/// lines stay on the terminal as they are, and text written after them
/// follows them.
#[derive(Debug)]
pub(crate) struct Redrawn<'f, W: Write> {
    footer: &'f Footer<W>,
    pending: Vec<u8>,
}

impl<W: Write> Write for Redrawn<'_, W> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.pending.extend_from_slice(buf);
        Ok(buf.len())
    }

    /// Replaces the footer's lines with those written since the last
    /// flush, if any were.
    fn flush(&mut self) -> io::Result<()> {
        let mut state = self.footer.lock();
        if !self.pending.is_empty() {
            state.erase()?;
            state.out.write_all(&self.pending)?;
            state.shown = std::mem::take(&mut self.pending);
        }
        state.out.flush()
    }
}

impl<W: Write> Drop for Redrawn<'_, W> {
    /// Leaves the footer's lines where they are, as ordinary text.
    fn drop(&mut self) {
        self.footer.lock().shown.clear();
    }
}

#[cfg(test)]
mod tests {
    use std::io::Write;

    use super::Footer;

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
}
