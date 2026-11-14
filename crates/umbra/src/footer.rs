//! The status line's place at the bottom of a terminal (arc42 §8.10), with
//! `indicatif`: log events print above it, and it is redrawn below them,
//! each line cut to the terminal's width.

use std::io::{self, Write};

use indicatif::{MultiProgress, ProgressBar, ProgressDrawTarget, ProgressStyle};

/// A terminal whose last lines, the footer, are redrawn in place while
/// other text scrolls above them. Log events are written through
/// `&Footer`; the footer's lines through [`Footer::redrawn`]. Where the
/// terminal is hidden, as stderr is when it is not a terminal, log events
/// go straight to stderr and the footer shows nothing.
#[derive(Debug)]
pub struct Footer {
    lines: MultiProgress,
}

impl Footer {
    /// A footer on `terminal`, showing nothing yet.
    #[must_use]
    pub fn new(terminal: ProgressDrawTarget) -> Self {
        Self {
            lines: MultiProgress::with_draw_target(terminal),
        }
    }

    /// Whether the footer shows nothing: its terminal is not one, or one
    /// that cannot move its cursor (`TERM=dumb`).
    #[must_use]
    pub fn is_hidden(&self) -> bool {
        self.lines.is_hidden()
    }

    /// The writer of the footer: each flush replaces what the footer shows
    /// with the lines written since the last.
    #[must_use]
    pub fn redrawn(&self) -> Redrawn<'_> {
        Redrawn {
            footer: self,
            lines: Vec::new(),
            pending: Vec::new(),
        }
    }
}

/// Log events: each write is whole lines, printed above the footer.
impl Write for &Footer {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        if self.is_hidden() {
            io::stderr().write_all(buf)?;
        } else {
            let text = String::from_utf8_lossy(buf);
            self.lines
                .println(text.strip_suffix('\n').unwrap_or(&text))?;
        }
        Ok(buf.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        io::stderr().flush()
    }
}

/// The writer of a [`Footer`]'s lines. Once dropped, its last lines stay
/// on the terminal as they are, and text written after them follows them.
#[derive(Debug)]
pub struct Redrawn<'f> {
    footer: &'f Footer,
    /// One per line shown.
    lines: Vec<ProgressBar>,
    pending: Vec<u8>,
}

impl Redrawn<'_> {
    /// A new line at the bottom of the footer.
    fn add_line(&self) -> ProgressBar {
        let style = ProgressStyle::with_template("{wide_msg}").expect("a valid template");
        self.footer
            .lines
            .add(ProgressBar::no_length().with_style(style))
    }
}

impl Write for Redrawn<'_> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.pending.extend_from_slice(buf);
        Ok(buf.len())
    }

    /// Replaces the footer's lines with those written since the last
    /// flush, if any were.
    fn flush(&mut self) -> io::Result<()> {
        if self.pending.is_empty() {
            return Ok(());
        }
        let text = String::from_utf8_lossy(&self.pending).into_owned();
        self.pending.clear();
        let new: Vec<&str> = text.lines().collect();
        while self.lines.len() < new.len() {
            let line = self.add_line();
            self.lines.push(line);
        }
        for gone in self.lines.drain(new.len()..) {
            self.footer.lines.remove(&gone);
        }
        for (line, text) in self.lines.iter().zip(new) {
            line.set_message(text.to_owned());
        }
        // Updates come a few times a second: each is drawn, never skipped
        // by `indicatif`'s rate limit.
        if let Some(last) = self.lines.last() {
            last.force_draw();
        }
        Ok(())
    }
}

impl Drop for Redrawn<'_> {
    /// Leaves the footer's lines where they are, as ordinary text: they
    /// leave the footer and are printed again above it, now empty.
    fn drop(&mut self) {
        let shown: Vec<String> = self.lines.iter().map(ProgressBar::message).collect();
        for line in self.lines.drain(..) {
            line.finish_and_clear();
            self.footer.lines.remove(&line);
        }
        for text in shown {
            // Nothing is left to show it on if the terminal is closed.
            let _ = self.footer.lines.println(text);
        }
    }
}
