//! The status line's place at the bottom of a terminal (arc42 §8.10): log
//! events print above it, and it is redrawn below them.

use std::io::{self, Write};
use std::sync::{Mutex, MutexGuard, PoisonError};

/// A terminal whose last lines, the footer, are redrawn in place, while
/// other text scrolls above them. Log events are written through `&Footer`;
/// the footer through [`Footer::redrawn`].
#[derive(Debug)]
pub struct Footer<W> {
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
    pub fn new(out: W) -> Self {
        Self {
            state: Mutex::new(State {
                out,
                shown: Vec::new(),
            }),
        }
    }

    /// The writer of the footer: each flush replaces what the footer shows
    /// with what was written since the last.
    pub fn redrawn(&self) -> Redrawn<'_, W> {
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
pub struct Redrawn<'f, W: Write> {
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
