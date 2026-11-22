// The wire format, and nothing else.
//
// PROTOCOL.md is the contract; this file is its shape in code. It knows no
// librespot types and needs no network, which is what makes it the part worth
// reading twice: a field that is off by one tab here shifts every track row on
// the player's screen.
//
// The rules that shape it, from the player's parser
// (sonix-player/src/system/streaming/spotify.c):
//
//   * the command word ends at the first space or tab;
//   * STATE is key=value pairs, any order, spaces or tabs between them;
//   * every other line is TAB-separated, positionally. A field may be empty
//     and must still occupy its place -- a cover URL often is -- so nothing
//     here may split on runs of whitespace;
//   * a field never contains a tab or a newline. flatten() is what guarantees
//     it: a track called "Blue\tHour" would otherwise move the duration into
//     the artist's slot.
//
// The player reads at most 128 lines and 24576 bytes of a reply. The caps
// below leave room for the END line, so the player always sees whole lines: a
// list longer than one page is asked for again with a bigger offset.

pub const REPLY_LINES_MAX: usize = 120;
pub const REPLY_BYTES_MAX: usize = 20_000;

/// The 32-byte magic that opens a PCM channel, then `44100 2 16\n`.
pub const PCM_MAGIC: [u8; 32] = {
    let mut m = [0u8; 32];
    let tag = b"SNXPCM1";
    let mut i = 0;
    while i < tag.len() {
        m[i] = tag[i];
        i += 1;
    }
    m
};

/// A field may not contain a tab or a line break. Spaces are fine, and a title
/// keeps its own; only the separators are replaced.
pub fn flatten(s: &str) -> String {
    s.replace(['\t', '\n', '\r'], " ")
}

/// One reply: the data lines, then END (or a single ERR).
pub struct Reply {
    lines: Vec<String>,
}

impl Default for Reply {
    fn default() -> Self {
        Self::new()
    }
}

impl Reply {
    pub fn new() -> Self {
        Reply { lines: Vec::new() }
    }

    pub fn line(&mut self, s: String) {
        if self.lines.len() < REPLY_LINES_MAX {
            self.lines.push(s);
        }
    }

    /// `END` for a command that has nothing to say.
    pub fn done(&mut self) {
        self.lines.push("END".into());
    }

    /// `END` or `ERR <why>`, whichever the work earned.
    pub fn verdict(&mut self, ok: bool, why: &str) {
        if ok {
            self.done();
        } else {
            self.line(format!("ERR {}", flatten(why)));
        }
    }

    pub fn error(why: &str) -> Self {
        let mut r = Reply::new();
        r.line(format!("ERR {}", flatten(why)));
        r
    }

    /// The bytes to put on the wire. Always ends with exactly one END line,
    /// even if the caps cut the data short -- a reply the player cannot see the
    /// end of is a reply it treats as a timeout.
    pub fn encode(&self) -> String {
        let mut out = String::new();
        let mut bytes = 0usize;
        let mut ended = false;
        for line in &self.lines {
            if bytes + line.len() + 1 > REPLY_BYTES_MAX {
                break;
            }
            bytes += line.len() + 1;
            out.push_str(line);
            out.push('\n');
            if line == "END" || line.starts_with("ERR ") {
                ended = true;
            }
        }
        if !ended {
            out.push_str("END\n");
        }
        out
    }
}

/// STATE, and TRACK when something is loaded. The fields the player does not
/// recognise are ignored, so a newer daemon can add one without breaking an
/// older player -- that is why this takes a struct rather than six arguments.
pub struct Status {
    pub state: u32, // 0 unavailable, 1 ready, 2 loading, 3 playing, 4 paused, 5 stopped
    pub position_ms: i64,
    pub duration_ms: i64,
    pub shuffle: bool,
    pub repeat: u32, // 0 off, 1 the queue, 2 the track
    pub volume: i64,
    pub index: i64,
    pub count: i64,
    pub account: String,
    pub track: Option<Track>,
}

pub struct Track {
    pub id: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub cover_url: String,
    pub duration_ms: i64,
}

impl Clone for Track {
    fn clone(&self) -> Self {
        Track {
            id: self.id.clone(),
            title: self.title.clone(),
            artist: self.artist.clone(),
            album: self.album.clone(),
            cover_url: self.cover_url.clone(),
            duration_ms: self.duration_ms,
        }
    }
}

impl Track {
    /// `T\t<id>\t<title>\t<artist>\t<album>\t<cover>\t<duration>`
    pub fn line(&self) -> String {
        format!(
            "T\t{}\t{}\t{}\t{}\t{}\t{}",
            flatten(&self.id),
            flatten(&self.title),
            flatten(&self.artist),
            flatten(&self.album),
            flatten(&self.cover_url),
            self.duration_ms
        )
    }
}

impl Status {
    pub fn encode(&self) -> Reply {
        let mut r = Reply::new();
        r.line(format!(
            "STATE state={} pos={} dur={} shuffle={} repeat={} volume={} index={} count={} account={}",
            self.state,
            self.position_ms,
            self.duration_ms,
            if self.shuffle { 1 } else { 0 },
            self.repeat,
            self.volume,
            self.index,
            self.count,
            flatten(&self.account),
        ));
        // TRACK is sent only while something is loaded (2..5). At 1 -- signed
        // in, nothing playing -- there is no track, and a TRACK line would give
        // the player a card with no words on it.
        if self.state >= 2 {
            if let Some(t) = self.track.as_ref() {
                r.line(format!(
                    "TRACK\t{}\t{}\t{}\t{}\t{}\t{}",
                    flatten(&t.id),
                    flatten(&t.title),
                    flatten(&t.artist),
                    flatten(&t.album),
                    flatten(&t.cover_url),
                    t.duration_ms
                ));
            }
        }
        r.done();
        r
    }
}

/// One page of a list: the list's own length, then the tracks, in order. An
/// unreadable track keeps its place as `?` -- the player draws that row as an
/// explanation, and its empty id is why tapping it plays nothing.
pub struct ListPage {
    pub total: usize,
    pub tracks: Vec<Option<Track>>,
    /// Set when the page could not be read at all: the reply is then one ERR
    /// line, because "no tracks" and "the request failed" are different things
    /// to the page that asked.
    pub error: Option<String>,
}

impl ListPage {
    pub fn failed(why: &str) -> Self {
        ListPage {
            total: 0,
            tracks: Vec::new(),
            error: Some(why.to_string()),
        }
    }

    pub fn encode(&self) -> Reply {
        if let Some(why) = self.error.as_ref() {
            return Reply::error(why);
        }
        let mut r = Reply::new();
        r.line(format!("N\t{}", self.total));
        for t in &self.tracks {
            match t {
                Some(track) => r.line(track.line()),
                None => r.line("?\tcould not be read".into()),
            }
        }
        r.done();
        r
    }
}

pub struct PlaylistRow {
    pub id: String,
    pub name: String,
    pub owner: String,
    pub count: usize,
}

impl PlaylistRow {
    pub fn line(&self) -> String {
        format!(
            "P\t{}\t{}\t{}\t{}",
            flatten(&self.id),
            flatten(&self.name),
            flatten(&self.owner),
            self.count
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // These run with `cargo test` and pin the three things a daemon author gets
    // wrong: a tab inside a field, a reply with no END, and an empty cover
    // shifting the duration.

    #[test]
    fn a_tab_in_a_title_does_not_shift_the_fields() {
        let t = Track {
            id: "abc".into(),
            title: "Blue\tHour".into(),
            artist: "Marta Vey".into(),
            album: "Northern Lines".into(),
            cover_url: "".into(),
            duration_ms: 213_000,
        };
        let line = t.line();
        assert_eq!(line.matches('\t').count(), 6);
        assert_eq!(line, "T\tabc\tBlue Hour\tMarta Vey\tNorthern Lines\t\t213000");
    }

    #[test]
    fn an_empty_cover_keeps_its_place() {
        let line = Track {
            id: "i".into(),
            title: "t".into(),
            artist: "a".into(),
            album: "b".into(),
            cover_url: String::new(),
            duration_ms: 1,
        }
        .line();
        // Six tabs, so the duration is the seventh field even with no artwork.
        assert_eq!(line.split('\t').count(), 7);
        assert_eq!(line.split('\t').nth(6).unwrap(), "1");
    }

    #[test]
    fn a_reply_always_ends_with_end() {
        let r = Reply::error("no daemon");
        assert_eq!(r.encode(), "ERR no daemon\n");
        let empty = Reply::new();
        assert_eq!(empty.encode(), "END\n");
    }

    #[test]
    fn a_long_reply_is_cut_before_the_player_stops_reading() {
        let mut page = ListPage {
            total: 10_000,
            tracks: Vec::new(),
            error: None,
        };
        for i in 0..10_000 {
            page.tracks.push(Some(Track {
                id: format!("id{}", i),
                title: "a title long enough that a thousand of them do not fit".repeat(2),
                artist: "artist".into(),
                album: "album".into(),
                cover_url: String::new(),
                duration_ms: 1000,
            }));
        }
        let text = page.encode().encode();
        assert!(text.lines().count() <= REPLY_LINES_MAX + 1);
        assert!(text.ends_with("END\n"));
    }

    #[test]
    fn status_has_no_track_before_anything_is_loaded() {
        let s = Status {
            state: 1,
            position_ms: 0,
            duration_ms: 0,
            shuffle: false,
            repeat: 0,
            volume: 50,
            index: 1,
            count: 0,
            account: "you".into(),
            track: None,
        };
        let text = s.encode().encode();
        assert!(text.contains("state=1"));
        assert!(!text.contains("TRACK"));
        assert!(text.ends_with("END\n"));
    }
}
