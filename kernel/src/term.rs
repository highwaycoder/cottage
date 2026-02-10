use limine::framebuffer::Framebuffer;

use crate::math::Vector2;

const TOSHSAT_FONT: &[u8] = include_bytes!("TOSH-SAT.F16");

pub struct TerminalContext<'a> {
    cols: u16,
    rows: u16,
    cursor_x: u16,
    cursor_y: u16,
    framebuffer: Framebuffer<'a>,
}

pub enum CursorMotion {
    RelPlus(u16),
    RelMinus(u16),
    Abs(u16),
}

impl TerminalContext<'_> {
    pub fn new<'a>(framebuffer: Framebuffer<'a>) -> TerminalContext<'a> {
        TerminalContext {
            cols: 80,
            rows: 20,
            cursor_x: 0,
            cursor_y: 0,
            framebuffer,
        }
    }

    pub fn move_cursor(&mut self, col: CursorMotion, row: CursorMotion) {
        match col {
            CursorMotion::Abs(x) => {
                self.cursor_x = x;
            }
            CursorMotion::RelPlus(x) => {
                self.cursor_x = self.cursor_x + x;
            }
            CursorMotion::RelMinus(x) => {
                self.cursor_x = self.cursor_x - x;
            }
        }
        match row {
            CursorMotion::Abs(y) => {
                self.cursor_y = y;
            }
            CursorMotion::RelPlus(y) => {
                self.cursor_y = self.cursor_y + y;
            }
            CursorMotion::RelMinus(y) => {
                self.cursor_y = self.cursor_y - y;
            }
        }
    }

    fn plotchar(&mut self, ch: char) {
        let startaddr = self.framebuffer.addr();
        // compute coordinate for plotting the character
        let glyph_size = Vector2 {
            x: self.framebuffer.width() / self.cols as u64,
            y: self.framebuffer.height() / self.rows as u64,
        };
        let top = self.cursor_y * glyph_size.y * self.framebuffer.pitch;
        let bottom = top + glyph_size.y;
        let left = self.cursor_x * glyph_size.x;
        let right = left + glyph_size.x;

    }

    pub fn printk(&mut self, msg: &str) {
        let idx: usize = 0;
        while ({ idx += 1; idx } < msg.len()) {
            match msg.chars().nth(idx) {
                Some('\n') => {
                    self.move_cursor(CursorMotion::Abs(0), CursorMotion::RelPlus(1));
                }
                Some(ch) => {
                    self.plotchar(ch);
                }
                None => {
                    return;
                }
            }
        }
    }
}
