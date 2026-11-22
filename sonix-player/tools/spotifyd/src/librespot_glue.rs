// The librespot half: session, browsing, playback, and the sink that writes to
// the player's socket.
//
// THIS FILE HAS NEVER BEEN COMPILED IN THIS REPOSITORY. There is no Rust
// toolchain where this firmware is built, and the daemon runs on your computer,
// not on the player. It is checked in because it is the recipe: the shape of
// each call, which librespot type it needs, and what the daemon does with the
// answer. `cargo build` will tell you where librespot's API has moved since
// 0.6; the protocol in protocol.rs and the contract in PROTOCOL.md will not
// have moved, and those are the parts the player is built against.
//
// The mapping to the player, in one table:
//
//   LIKED    -> Playlist::get_liked(session)                 -> ListPage
//   PLAYLISTS-> Playlist::get_playlists(session)             -> PlaylistRow
//   PLAYLIST -> Playlist::get(session, id)                   -> ListPage
//   SEARCH   -> Track::search(session, query, 0, 50)         -> ListPage
//   PLAY     -> Player::load(Track, start, position)         -> END
//   PLAYCTX  -> Player::set_available_context / set_playing_context, then load
//   PCM      -> the Sink below, which librespot calls on its decode thread
//
// Every handler is `async`; main.rs drives them on a small current-thread
// tokio runtime, one command at a time per connection. Browsing one page is a
// handful of metadata requests and takes about a second on a home network.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use librespot::core::authentication::Credentials;
use librespot::core::cache::Cache;
use librespot::core::config::SessionConfig;
use librespot::core::session::Session;
use librespot::core::spotify_id::SpotifyId;
use librespot::metadata::{Album, Artist, Metadata, Playlist, Track};
use librespot::playback::audio_backend::{Sink, SinkError, SinkResult};
use librespot::playback::config::{Bitrate, PlayerConfig};
use librespot::playback::player::{Player, PlayerEvent};

use crate::protocol::{ListPage, PlaylistRow, Reply, Status, Track as WireTrack};
use crate::Library;

/// What every handler reads and the status page reports. Atomics because
/// librespot's event callback runs on the decode thread and the status command
/// runs on a connection thread: neither waits for the other.
pub struct Shared {
    /// The player's socket while a PCM channel is open. `None` between
    /// channels, which is also how the sink knows to drop frames decoded in
    /// the gap rather than block the decoder.
    pub pcm: Mutex<Option<Box<dyn std::io::Write + Send>>>,
    pub playing: AtomicBool,
    pub position_ms: AtomicI64,
    pub duration_ms: AtomicI64,
    pub state: AtomicU64, // 0..5, the protocol's numbering
    pub shuffle: AtomicBool,
    pub repeat: AtomicU64,
    pub volume: AtomicI64,
    pub index: AtomicI64,
    pub count: AtomicI64,
    pub now: Mutex<Option<WireTrack>>,
}

impl Shared {
    pub fn new() -> Arc<Self> {
        Arc::new(Shared {
            pcm: Mutex::new(None),
            playing: AtomicBool::new(false),
            position_ms: AtomicI64::new(0),
            duration_ms: AtomicI64::new(0),
            state: AtomicU64::new(1), // signed in, nothing loaded
            shuffle: AtomicBool::new(false),
            repeat: AtomicU64::new(0),
            volume: AtomicI64::new(50),
            index: AtomicI64::new(0),
            count: AtomicI64::new(0),
            now: Mutex::new(None),
        })
    }
}

/// librespot's sink, pointed at the player instead of at ALSA.
///
/// librespot hands us interleaved little-endian frames; they go on the socket
/// as they are, because the player asked for exactly this shape in the PCM
/// header. When no channel is open the frames are dropped: the track carries on
/// and the player picks it up by seeking when it reopens.
pub struct SocketSink {
    pub shared: Arc<Shared>,
}

impl Sink for SocketSink {
    fn start(&mut self) -> SinkResult<()> {
        Ok(())
    }

    fn stop(&mut self) -> SinkResult<()> {
        *self.shared.pcm.lock().unwrap() = None;
        Ok(())
    }

    fn write(&mut self, data: &[u8]) -> SinkResult<()> {
        use std::io::Write;
        let mut guard = self.shared.pcm.lock().map_err(|_| SinkError::NotConnected)?;
        if let Some(stream) = guard.as_mut() {
            // A broken pipe is the player going away mid-track: clear the
            // channel and let the connection thread close it. Not an error the
            // phone should see.
            if stream.write_all(data).is_err() {
                *guard = None;
            }
        }
        Ok(())
    }

    fn close(&mut self) {}
}

/// The process's one runtime, and the one library behind every connection.
pub struct Daemon {
    session: Session,
    player: Arc<Player>,
    pub shared: Arc<Shared>,
    account: String,
    runtime: tokio::runtime::Runtime,
}

/// Connect once at startup. The return type is what main.rs keeps: a Library,
/// so the protocol layer never names a librespot type.
pub fn connect(cfg: &crate::Config) -> Result<Arc<dyn Library>, Box<dyn std::error::Error>> {
    let runtime = tokio::runtime::Builder::new_multi_thread().enable_all().build()?;
    let inner = runtime.block_on(open(cfg))?;
    Ok(Arc::new(Daemon {
        session: inner.session,
        player: inner.player,
        shared: inner.shared,
        account: inner.account,
        runtime,
    }))
}

struct Opened {
    session: Session,
    player: Arc<Player>,
    shared: Arc<Shared>,
    account: String,
}

/// The real work of connecting: the session, the player, and the event loop
/// that keeps `shared` current. The event loop runs for the life of the
/// process; there is one player, so there is one loop.
async fn open(cfg: &crate::Config) -> Result<Opened, Box<dyn std::error::Error>> {
    let cache = Cache::new(Some(cfg.cache.as_ref()), None, None, None)?;
    let session = Session::new(SessionConfig::default(), Some(cache.clone()));

    // librespot reads the credentials `librespot --cache <dir> --cache-tool
    // login` saved in the cache directory. There is no password in this
    // program and no sign-in on the player.
    let session = session
        .connect(Credentials::with_password("", ""), true)
        .await?;

    let shared = Shared::new();
    let player = Player::new(
        PlayerConfig {
            bitrate: match cfg.bitrate {
                96 => Bitrate::Bitrate96,
                160 => Bitrate::Bitrate160,
                _ => Bitrate::Bitrate320,
            },
            ..Default::default()
        },
        Arc::new(NoMixer::default()),
        cache,
        {
            let shared = shared.clone();
            move || Box::new(SocketSink { shared }) as Box<dyn Sink>
        },
    );

    // The event loop. It translates librespot's vocabulary into the
        // protocol's state numbers once, here, so no other code has to guess.
        {
            let shared = shared.clone();
            let player = player.clone();
            let mut events = player.get_player_event_channel();
            std::thread::spawn(move || loop {
                match events.recv() {
                    Ok(PlayerEvent::Playing { .. }) => {
                        shared.state.store(3, Ordering::SeqCst);
                        shared.playing.store(true, Ordering::SeqCst);
                    }
                    Ok(PlayerEvent::Paused { .. }) => {
                        shared.state.store(4, Ordering::SeqCst);
                        shared.playing.store(false, Ordering::SeqCst);
                    }
                    Ok(PlayerEvent::Stopped { .. }) => {
                        shared.state.store(5, Ordering::SeqCst);
                        shared.playing.store(false, Ordering::SeqCst);
                    }
                    Ok(PlayerEvent::Loading { .. }) => {
                        shared.state.store(2, Ordering::SeqCst);
                    }
                    Ok(PlayerEvent::TimeToPreloadNextTrack { .. }) => {}
                    Ok(PlayerEvent::EndOfTrack { .. }) => {
                        // The protocol's REPEAT 2 is one track over and over;
                        // the player leaves the queue alone and this is what
                        // closes the loop.
                        if shared.repeat.load(Ordering::SeqCst) == 2 {
                            let _ = player.seek(0);
                            player.play();
                        }
                    }
                    Ok(PlayerEvent::PositionChanged { position, .. })
                    | Ok(PlayerEvent::PositionCorrection { position, .. })
                    | Ok(PlayerEvent::Seeked { position, .. }) => {
                        shared.position_ms.store(position as i64, Ordering::SeqCst);
                    }
                    Ok(PlayerEvent::TrackChanged { audio_item }) => {
                        let mut now = shared.now.lock().unwrap();
                        if let Some(track) = &now {
                            // Keep the words the browse page already fetched:
                            // TrackChanged carries the audio item, not the
                            // metadata, and asking again here would put a
                            // second of blank title on the Now Playing sheet.
                            let _ = track;
                        }
                        let _ = audio_item;
                        drop(now);
                    }
                    Ok(PlayerEvent::Unavailable { .. }) => {
                        shared.state.store(4, Ordering::SeqCst);
                    }
                    Ok(_) => {}
                    // The player was dropped: nothing left to watch.
                    Err(_) => break,
                }
            });
        }

    Ok(Opened {
        session,
        player,
        shared,
        account: String::new(),
    })
}

impl Daemon {
    /// Fill in a wire track from librespot's metadata, then hand the track to
    /// the player. The metadata is fetched first so the status page has words
    /// before the audio starts -- on a slow link the other order shows a
    /// hexadecimal id for a second.
    fn play_track(&self, id: &str) -> Result<(), Box<dyn std::error::Error>> {
        let uri = SpotifyId::from_base62(id)?;
        let track = Track::get(&self.session, &uri).await?;

        let mut artist = String::new();
        if let Some(first) = track.artists.first() {
            if let Ok(a) = Artist::get(&self.session, &first.id).await {
                artist = a.name;
            }
        }
        let mut album_name = String::new();
        let mut cover = String::new();
        if let Some(album) = track.album.as_ref() {
            if let Ok(a) = Album::get(&self.session, &album.id).await {
                album_name = a.name.clone();
                if let Some(image) = a.covers.first() {
                    cover = image.url.clone();
                }
            }
        }

        let wire = WireTrack {
            id: id.to_string(),
            title: track.name.clone(),
            artist,
            album: album_name,
            cover_url: cover,
            duration_ms: track.duration as i64,
        };
        self.shared.duration_ms.store(wire.duration_ms, Ordering::SeqCst);
        *self.shared.now.lock().unwrap() = Some(wire);

        self.player.load(track, true, 0)?;
        Ok(())
    }

    /// One page of Liked Songs.
    fn liked_page(&self, offset: usize, limit: usize) -> Result<ListPage, String> {
        let list = Playlist::get_liked(&self.session).await.map_err(|e| e.to_string())?;
        self.page(&list, offset, limit).await
    }

    /// One page of a playlist's tracks.
    fn playlist_page(&self, id: &str, offset: usize, limit: usize) -> Result<ListPage, String> {
        let uri = SpotifyId::from_base62(id).map_err(|e| e.to_string())?;
        let list = Playlist::get(&self.session, &uri).await.map_err(|e| e.to_string())?;
        self.page(&list, offset, limit).await
    }

    fn playlist_rows(&self) -> Result<Vec<PlaylistRow>, String> {
        let lists = Playlist::get_playlists(&self.session).await.map_err(|e| e.to_string())?;
        Ok(lists
            .into_iter()
            .map(|l| crate::protocol::PlaylistRow {
                id: l.id.to_base62(),
                name: l.name,
                owner: l.owner.name.unwrap_or_default(),
                count: l.tracks.len(),
            })
            .collect())
    }

    fn search_page(&self, query: &str) -> Result<ListPage, String> {
        let page = librespot::metadata::page::Page::<Track>::get(
            &self.session,
            format!("/search?q={}&type=track&limit=50", urlencode(query)),
        )
        .await
        .map_err(|e| e.to_string())?;
        let mut out = Vec::with_capacity(page.items.len());
        let mut artists: HashMap<SpotifyId, String> = HashMap::new();
        let mut albums: HashMap<SpotifyId, (String, String)> = HashMap::new();
        for track in page.items {
            out.push(self.describe(&track, &mut artists, &mut albums).await);
        }
        Ok(crate::protocol::ListPage { total: out.len(), tracks: out })
    }

    /// The shared body of the three list commands: skip, take, then describe
    /// each track. A track that cannot be read keeps its slot as `None`.
    async fn page(&self, list: &Playlist, offset: usize, limit: usize) -> Result<ListPage, String> {
        let mut artists: HashMap<SpotifyId, String> = HashMap::new();
        let mut albums: HashMap<SpotifyId, (String, String)> = HashMap::new();
        let total = list.tracks.len();
        let mut out = Vec::new();
        for entry in list.tracks.iter().skip(offset).take(limit) {
            match Track::get(&self.session, &entry.id).await {
                Ok(track) => out.push(self.describe(&track, &mut artists, &mut albums).await),
                Err(_) => out.push(None),
            }
        }
        Ok(crate::protocol::ListPage { total, tracks: out })
    }

    /// Track + artist + album + cover, with the last three cached: a page of
    /// thirty tracks by one artist is two lookups, not sixty.
    pub async fn describe(
        &self,
        track: &Track,
        artists: &mut HashMap<SpotifyId, String>,
        albums: &mut HashMap<SpotifyId, (String, String)>,
    ) -> Option<WireTrack> {
        let mut artist = String::new();
        if let Some(first) = track.artists.first() {
            if let Some(name) = artists.get(&first.id) {
                artist = name.clone();
            } else if let Ok(a) = Artist::get(&self.session, &first.id).await {
                artist = a.name.clone();
                artists.insert(first.id, a.name);
            }
        }
        let mut album = String::new();
        let mut cover = String::new();
        if let Some(al) = track.album.as_ref() {
            if let Some(hit) = albums.get(&al.id) {
                album = hit.0.clone();
                cover = hit.1.clone();
            } else if let Ok(a) = Album::get(&self.session, &al.id).await {
                album = a.name.clone();
                if let Some(image) = a.covers.first() {
                    cover = image.url.clone();
                }
                albums.insert(al.id, (a.name, cover.clone()));
            }
        }
        Some(WireTrack {
            id: track.id.to_base62(),
            title: track.name.clone(),
            artist,
            album,
            cover_url: cover,
            duration_ms: track.duration as i64,
        })
    }
}

}

impl Library for Daemon {
    fn status(&self) -> Status {
        let s = &self.shared;
        Status {
            state: s.state.load(Ordering::SeqCst) as u32,
            position_ms: s.position_ms.load(Ordering::SeqCst),
            duration_ms: s.duration_ms.load(Ordering::SeqCst),
            shuffle: s.shuffle.load(Ordering::SeqCst),
            repeat: s.repeat.load(Ordering::SeqCst) as u32,
            volume: s.volume.load(Ordering::SeqCst),
            index: s.index.load(Ordering::SeqCst) + 1,
            count: s.count.load(Ordering::SeqCst),
            account: self.account.clone(),
            // A clone rather than a borrow: the lock must not be held while the
            // reply is built, or a decode thread updating the title waits on a
            // connection writing a status line.
            track: s.now.lock().unwrap().clone(),
        }
    }

    fn liked(&self, offset: usize, limit: usize) -> ListPage {
        self.runtime
            .block_on(self.liked_page(offset, limit))
            .unwrap_or_else(|e| ListPage::failed(&e))
    }

    fn playlist(&self, id: &str, offset: usize, limit: usize) -> ListPage {
        self.runtime
            .block_on(self.playlist_page(id, offset, limit))
            .unwrap_or_else(|e| ListPage::failed(&e))
    }

    fn playlists(&self) -> Reply {
        match self.runtime.block_on(self.playlist_rows()) {
            Ok(rows) => {
                let mut r = Reply::new();
                for row in rows {
                    r.line(row.line());
                }
                r.done();
                r
            }
            Err(e) => Reply::error(&e),
        }
    }

    fn search(&self, query: &str) -> ListPage {
        self.runtime
            .block_on(self.search_page(query))
            .unwrap_or_else(|e| ListPage::failed(&e))
    }

    fn play(&self, id: &str) -> Reply {
        let mut r = Reply::new();
        r.verdict(self.play_track(id).is_ok(), "could not load the track");
        r
    }

    fn play_context(&self, kind: &str, context: &str, id: &str) -> Reply {
        // The queue belongs to librespot, so the context is set before the
        // track: "next" on the V1 then means what it means on the phone.
        let ok = self.runtime.block_on(async {
            if kind == "playlist" {
                if let Ok(plid) = SpotifyId::from_base62(context) {
                    if let Ok(list) = Playlist::get(&self.session, &plid).await {
                        self.player.set_available_context(list, 0);
                    }
                }
            }
            self.play_track(id).is_ok()
        });
        let mut r = Reply::new();
        r.verdict(ok, "could not start the context");
        r
    }

    fn toggle(&self) {
        if self.shared.playing.load(Ordering::SeqCst) {
            self.player.pause();
        } else {
            self.player.play();
        }
    }

    fn resume(&self) {
        self.player.play();
    }

    fn pause(&self) {
        self.player.pause();
    }

    fn next(&self) {
        self.player.next();
    }

    fn previous(&self) {
        self.player.prev();
    }

    fn seek(&self, ms: u32) {
        self.player.seek(ms);
    }

    fn shuffle(&self, on: bool) {
        self.shared.shuffle.store(on, Ordering::SeqCst);
    }

    fn repeat(&self, mode: u32) {
        self.shared.repeat.store(mode as u64, Ordering::SeqCst);
    }

    fn volume(&self, percent: i64) {
        self.shared.volume.store(percent, Ordering::SeqCst);
        self.player.set_volume(percent.clamp(0, 100) as u16);
    }

    fn queue(&self) -> Reply {
        let mut r = Reply::new();
        r.line(format!(
            "Q\t{}\t{}",
            self.shared.index.load(Ordering::SeqCst),
            self.shared.count.load(Ordering::SeqCst)
        ));
        r.done();
        r
    }

    fn auth(&self) -> Reply {
        let mut r = Reply::new();
        if self.account.is_empty() {
            r.line("AUTH none".into());
        } else {
            r.line(format!("AUTH ok {}", self.account));
        }
        r.done();
        r
    }

    fn open_pcm(&self, stream: Box<dyn std::io::Write + Send>) {
        *self.shared.pcm.lock().unwrap() = Some(stream);
    }

    fn close_pcm(&self) {
        *self.shared.pcm.lock().unwrap() = None;
    }
}

/// librespot wants a mixer; the player's own volume control is the real one,
/// so this one only remembers the number the protocol reports.
#[derive(Default)]
pub struct NoMixer {
    volume: AtomicI64,
}

impl librespot::playback::mixer::Mixer for NoMixer {
    fn set_volume(&self, volume: u16) {
        self.volume.store(volume as i64, Ordering::SeqCst);
    }

    fn volume(&self) -> u16 {
        self.volume.load(Ordering::SeqCst) as u16
    }
}

/// Spotify's search endpoint takes a percent-encoded query; the player sends
/// spaces, which is right for the human and wrong for the URL.
fn urlencode(s: &str) -> String {
    let mut out = String::new();
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => out.push(b as char),
            b' ' => out.push('+'),
            _ => out.push_str(&format!("%{:02X}", b)),
        }
    }
    out
}
