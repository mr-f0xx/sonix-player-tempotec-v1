// The librespot half: session, browsing, playback, and the sink that writes to
// the player's socket.
//
// This file was rewritten against librespot 0.7's actual source, not against an
// idea of it. The first version called Playlist::get_liked, Playlist::get_
// playlists, Track::search, Player::next and player.set_available_context --
// none of which exist in 0.6, 0.7 or 0.8. That is why it never compiled, and no
// amount of pinning would have fixed it.
//
// What 0.7 really offers, and what this file now uses:
//
//   Metadata::get(session, &SpotifyId)   -> Track / Album / Artist / Playlist
//   session.spclient().get_rootlist()     -> the user's playlists (protobuf)
//   session.spclient().request_as_json()  -> search, the one JSON endpoint
//   Player::load(SpotifyId, play, pos)    -> one track, no queue
//
// The queue is ours. librespot's Player plays a single track and says when it
// ends; next/previous/shuffle/repeat are the daemon's bookkeeping over a Vec of
// SpotifyIds. That is less clever than Spirc and far more predictable, and it
// is what makes PLAYCTX mean the same thing on the player as on the phone.
//
// The mapping to the player, in one table:
//
//   LIKED    -> the likestate playlist, paged           -> ListPage
//   PLAYLISTS-> spclient().get_rootlist()               -> PlaylistRow
//   PLAYLIST -> Metadata::get::<Playlist>(id)           -> ListPage
//   SEARCH   -> spclient().request_as_json("/search")   -> ListPage
//   PLAY     -> Player::load(track, true, 0)            -> END
//   PLAYCTX  -> fill the queue around a track, load it  -> END
//   NEXT/PREV-> move the queue cursor and load          -> END
//   PCM      -> the Sink below, called on the decode thread
//
// Browse handlers are `async` and run on the process's one runtime through
// Library's block_on; the sink and the event loop are synchronous, on their own
// librespot threads.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use librespot::core::authentication::Credentials;
use librespot::core::cache::Cache;
use librespot::core::config::SessionConfig;
use librespot::core::session::Session;
use librespot::core::spotify_id::SpotifyId;
use librespot::metadata::playlist::list::SelectedListContent;
use librespot::metadata::{Album, Artist, Metadata, Playlist, Track};
use librespot::playback::audio_backend::{Sink, SinkError, SinkResult};
use librespot::playback::config::{AudioFormat, Bitrate, PlayerConfig};
use librespot::playback::convert::Converter;
use librespot::playback::decoder::AudioPacket;
use librespot::playback::mixer::NoOpVolume;
use librespot::playback::player::{Player, PlayerEvent};

use crate::Library;
use protocol::{ListPage, PlaylistRow, Reply, Status, Track as WireTrack};

/// The user's own Liked Songs live in a playlist whose id is their username:
/// `spotify:user:<name>:playlist`, the old "starred" convention that Spotify
/// kept for the library. It is not a special call, just a playlist id.
fn likestate_id(user: &str) -> Option<SpotifyId> {
    SpotifyId::from_uri(&format!("spotify:user:{}:playlist", user)).ok()
}

/// The largest cover of an album, as a URL the player can fetch itself. The
/// player has an HTTP client and a certificate store (it needs them for the
/// radio); handing it a URL rather than megabytes of image keeps the socket for
/// what it is for.
fn cover_url(album: &Album) -> String {
    // Chosen by pixel area, not by ImageSize: that enum is generated from the
    // protobuf at build time, so its variant names belong to the codegen, not
    // to a documented API -- and librespot itself never names one. width and
    // height are plain numbers on Image, and they say what the pick is for.
    let mut best: Option<&librespot::metadata::image::Image> = None;
    let mut best_area = -1i64;
    for image in album.covers.iter() {
        let area = image.width as i64 * image.height as i64;
        if area > best_area {
            best_area = area;
            best = Some(image);
        }
    }
    match best {
        Some(image) => match image.id.to_base16() {
            Ok(id) => format!("https://i.scdn.co/image/{}", id),
            Err(_) => String::new(),
        },
        None => String::new(),
    }
}

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
    /// The queue, oldest first, and the cursor into it. Ours, not librespot's:
    /// Player plays one track at a time and reports its end, and everything
    /// next/previous/shuffle do is here.
    pub queue: Mutex<Vec<SpotifyId>>,
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
            queue: Mutex::new(Vec::new()),
        })
    }
}

/// librespot's sink, pointed at the player instead of at ALSA.
///
/// librespot hands us decoded f64 samples and a converter; they leave as
/// interleaved little-endian bytes in the format the PCM header promised. When
/// no channel is open the frames are dropped: the track carries on and the
/// player picks it up by seeking when it reopens.
pub struct SocketSink {
    pub shared: Arc<Shared>,
    /// What the PCM header line promised the player. The sink is the only
    /// thing that can keep that promise, because it is the only thing that
    /// touches the samples.
    pub format: AudioFormat,
}

/// Interleaved little-endian bytes from a slice of samples, without `unsafe`:
/// zero-copy casts from &[i16] are the one thing std does not offer, and
/// writing the bytes out one field at a time is both safe and fast enough at
/// 44.1 kHz (a copy of a few KB per packet).
fn s16_le(samples: &[i16]) -> Vec<u8> {
    let mut out = Vec::with_capacity(samples.len() * 2);
    for s in samples {
        out.extend_from_slice(&s.to_le_bytes());
    }
    out
}

/// 24-bit samples in three bytes each, which is what S24_3LE means.
fn s24_3_le(samples: &[i32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(samples.len() * 3);
    for s in samples {
        let b = s.to_le_bytes();
        out.extend_from_slice(&b[..3]);
    }
    out
}

impl Sink for SocketSink {
    fn start(&mut self) -> SinkResult<()> {
        Ok(())
    }

    fn stop(&mut self) -> SinkResult<()> {
        *self.shared.pcm.lock().unwrap() = None;
        Ok(())
    }

    fn write(&mut self, packet: AudioPacket, converter: &mut Converter) -> SinkResult<()> {
        let mut guard = self.shared.pcm.lock().map_err(|_| SinkError::NotConnected)?;
        let stream = match guard.as_mut() {
            Some(s) => s,
            None => return Ok(()), // no player listening: drop the frame
        };

        let written = match (self.format, packet) {
            (AudioFormat::S24_3, AudioPacket::Samples(samples)) => {
                let out = converter.f64_to_s24(&samples);
                stream.write_all(&s24_3_le(&out))
            }
            (_, AudioPacket::Samples(samples)) => {
                // S16 is the default, and it is also the honest fallback: the
                // header said 16 bits unless the config said otherwise.
                let out = converter.f64_to_s16(&samples);
                stream.write_all(&s16_le(&out))
            }
            (_, AudioPacket::Raw(samples)) => stream.write_all(&samples),
        };

        if written.is_err() {
            // A broken pipe is the player going away mid-track: clear the
            // channel and let the connection thread close it. Not an error the
            // phone should see.
            *guard = None;
        }
        Ok(())
    }
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

async fn open(cfg: &crate::Config) -> Result<Opened, Box<dyn std::error::Error>> {
    let cache = Cache::new(Some(&cfg.cache), None, None, None).ok();

    // SessionConfig is the session's own settings, not the Connect device's:
    // device_name belongs to spirc, which this daemon does not run. Nothing in
    // this config needs changing from the default, so it stays default.
    let session_config = SessionConfig::default();

    let session = Session::new(session_config, cache.clone());

    // The cache holds the credentials a one-time `librespot --cache-tool login`
    // saved. There is no password in this program and no sign-in on the player.
    let username = std::env::var("SPOTIFYD_USERNAME").unwrap_or_default();
    if !username.is_empty() {
        // connect() fills the session in place and stores what it learned, so
        // there is no new session to bind -- the old binding was the bug, not
        // a missing assignment.
        session
            .connect(Credentials::with_password(&username, ""), true)
            .await?;
    }

    let account = session.username();

    let shared = Shared::new();
    // Player::new takes the session and a volume getter. NoOpVolume says "do
    // not attenuate": the player's own volume keys move its DAC's level, and
    // attenuating again here would make the two disagree.
    let bits = cfg.bits;
    let sink_shared = shared.clone();
    let player = Player::new(
        PlayerConfig {
            bitrate: match cfg.bitrate {
                96 => Bitrate::Bitrate96,
                160 => Bitrate::Bitrate160,
                _ => Bitrate::Bitrate320,
            },
            ..Default::default()
        },
        session.clone(),
        Box::new(NoOpVolume),
        move || {
            Box::new(SocketSink {
                shared: sink_shared.clone(),
                format: if bits == 24 { AudioFormat::S24_3 } else { AudioFormat::S16 },
            }) as Box<dyn Sink>
        },
    );

    // The event loop. It translates librespot's vocabulary into the protocol's
    // state numbers once, here, so no other code has to guess.
    {
        let shared = shared.clone();
        let mut events = player.get_player_event_channel();
        std::thread::spawn(move || loop {
            match events.blocking_recv() {
                Some(PlayerEvent::Playing { .. }) => {
                    shared.state.store(3, Ordering::SeqCst);
                    shared.playing.store(true, Ordering::SeqCst);
                }
                Some(PlayerEvent::Paused { .. }) => {
                    shared.state.store(4, Ordering::SeqCst);
                    shared.playing.store(false, Ordering::SeqCst);
                }
                Some(PlayerEvent::Stopped { .. }) => {
                    shared.state.store(5, Ordering::SeqCst);
                    shared.playing.store(false, Ordering::SeqCst);
                }
                Some(PlayerEvent::EndOfTrack { .. }) => {
                    // The queue's next track is loaded by the status poll: the
                    // event thread must not block on metadata requests, and
                    // this is the one place where a track boundary exists.
                    shared.state.store(5, Ordering::SeqCst);
                    shared.playing.store(false, Ordering::SeqCst);
                }
                Some(PlayerEvent::TimeToPreloadNextTrack { .. }) => {}
                Some(_) => {}
                None => break, // the player is gone; the daemon is shutting down
            }
        });
    }

    Ok(Opened {
        session,
        player,
        shared,
        account,
    })
}

impl Daemon {
    /// Fill in a wire track from librespot's metadata. The status page wants
    /// words before the audio starts, so this runs before Player::load.
    async fn describe(&self, id: &SpotifyId) -> Option<WireTrack> {
        let track = Track::get(&self.session, id).await.ok()?;
        let artist = track
            .artists
            .iter()
            .map(|a| a.name.clone())
            .collect::<Vec<_>>()
            .join(", ");
        Some(WireTrack {
            id: id.to_base62().unwrap_or_default(),
            title: track.name.clone(),
            artist,
            album: track.album.name.clone(),
            cover_url: cover_url(&track.album),
            duration_ms: track.duration as i64,
        })
    }

    /// The queue, replaced and then driven to `cursor`.
    fn set_queue(&self, ids: Vec<SpotifyId>, cursor: usize) {
        let count = ids.len() as i64;
        self.shared.count.store(count, Ordering::SeqCst);
        self.shared.index.store(cursor as i64, Ordering::SeqCst);
        *self.shared.queue.lock().unwrap() = ids;
    }

    /// Loads whatever the queue cursor points at. The one place a track starts.
    async fn load_cursor(&self) -> Result<(), String> {
        let (id, index, count) = {
            let queue = self.shared.queue.lock().unwrap();
            if queue.is_empty() {
                return Err("nothing in the queue".into());
            }
            let index = self.shared.index.load(Ordering::SeqCst).clamp(0, queue.len() as i64 - 1);
            (queue[index as usize], index, queue.len() as i64)
        };
        self.shared.index.store(index, Ordering::SeqCst);
        self.shared.count.store(count, Ordering::SeqCst);

        let wire = self.describe(&id).await;
        if let Some(w) = wire {
            self.shared.duration_ms.store(w.duration_ms, Ordering::SeqCst);
            *self.shared.now.lock().unwrap() = Some(w);
        }
        self.player.load(id, true, 0);
        Ok(())
    }

    /// Skip honouring repeat: `repeat=2` means the same track again.
    async fn skip(&self, forward: bool) -> Result<(), String> {
        let repeat = self.shared.repeat.load(Ordering::SeqCst);
        if repeat == 2 {
            return self.load_cursor().await;
        }
        let len = self.shared.queue.lock().unwrap().len() as i64;
        if len == 0 {
            return Err("nothing in the queue".into());
        }
        let mut index = self.shared.index.load(Ordering::SeqCst) + if forward { 1 } else { -1 };
        if index >= len {
            if repeat == 1 {
                index = 0; // repeat the queue
            } else {
                return Err("the end of the queue".into());
            }
        }
        if index < 0 {
            index = 0;
        }
        self.shared.index.store(index, Ordering::SeqCst);
        self.load_cursor().await
    }

    /// A page of one playlist's tracks, skipping `offset` and reading `limit`.
    /// A track that cannot be read keeps its slot as `None`, which the player
    /// draws as "could not be read" rather than silently shifting the list.
    async fn playlist_page(&self, list: &Playlist, offset: usize, limit: usize) -> ListPage {
        let ids: Vec<SpotifyId> = list.tracks().cloned().collect();
        let total = ids.len();
        let mut out = Vec::new();
        for id in ids.iter().skip(offset).take(limit) {
            out.push(self.describe(id).await);
        }
        ListPage {
            total,
            tracks: out,
            error: None,
        }
    }

    /// The user's playlists, from the rootlist. librespot hands the response
    /// back as protobuf bytes; SelectedListContent is the public type that
    /// parses them, and it is the same type a playlist request produces.
    async fn playlist_rows(&self) -> Result<Vec<PlaylistRow>, String> {
        let body = self
            .session
            .spclient()
            .get_rootlist(0, Some(200))
            .await
            .map_err(|e| e.to_string())?;
        let msg: <Playlist as Metadata>::Message =
            protobuf::Message::parse_from_bytes(&body).map_err(|e| e.to_string())?;
        let content = SelectedListContent::try_from(&msg).map_err(|e| e.to_string())?;

        let mut rows = Vec::new();
        // not contents.iter(): a PlaylistItemList is a struct, and the list of
        // entries is its `items` field.
        for item in content.contents.items.iter() {
            let id = &item.id;
            // One request per playlist for its name and length. A user with
            // fifty playlists pays fifty requests once per visit to the page;
            // the player caches the rows, so that is once, not per row scroll.
            if let Ok(list) = Playlist::get(&self.session, id).await {
                rows.push(PlaylistRow {
                    id: id.to_base62().unwrap_or_default(),
                    name: list.name().to_string(),
                    owner: list.id.username.clone(),
                    // contents.items.len(), not tracks().len(): tracks() is
                    // an iterator, and the Vec behind it is what has a length.
                    count: list.contents.items.len(),
                });
            }
        }
        Ok(rows)
    }

    /// Search. This is the one endpoint that answers JSON; the tracks come
    /// back as ids, and the words come from the metadata as usual.
    async fn search_page(&self, query: &str) -> Result<ListPage, String> {
        let endpoint = format!(
            "/search?q={}&type=track&limit=50",
            urlencode(query)
        );
        let body = self
            .session
            .spclient()
            .request_as_json(&http::Method::GET, &endpoint, None, None)
            .await
            .map_err(|e| e.to_string())?;

        let json: serde_json::Value =
            serde_json::from_slice(&body).map_err(|e| e.to_string())?;
        let items = json
            .pointer("/tracks/items")
            .or_else(|| json.pointer("/tracks/hits"))
            .and_then(|v| v.as_array())
            .cloned()
            .unwrap_or_default();

        let mut out = Vec::new();
        for item in items.iter() {
            // Both shapes carry the id as a spotify: URI.
            let uri = item
                .pointer("/uri")
                .or_else(|| item.pointer("/track/uri"))
                .and_then(|v| v.as_str())
                .unwrap_or("");
            if let Ok(id) = SpotifyId::from_uri(uri) {
                out.push(self.describe(&id).await);
            }
        }
        Ok(ListPage {
            total: out.len(),
            tracks: out,
            error: None,
        })
    }
}

/// Percent-encoding for a search query. librespot does not export one for this
/// endpoint, and a space in a query would otherwise cut the URL short.
fn urlencode(s: &str) -> String {
    let mut out = String::with_capacity(s.len() * 3);
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(b as char)
            }
            b' ' => out.push_str("%20"),
            _ => out.push_str(&format!("%{:02X}", b)),
        }
    }
    out
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
            track: s.now.lock().unwrap().clone(),
        }
    }

    fn liked(&self, offset: usize, limit: usize) -> ListPage {
        let user = self.account.clone();
        self.runtime.block_on(async {
            let id = match likestate_id(&user) {
                Some(id) => id,
                None => return ListPage::failed("no account is signed in"),
            };
            match Playlist::get(&self.session, &id).await {
                Ok(list) => self.playlist_page(&list, offset, limit).await,
                Err(e) => ListPage::failed(&format!("liked songs: {}", e)),
            }
        })
    }

    fn playlist(&self, id: &str, offset: usize, limit: usize) -> ListPage {
        let parsed = SpotifyId::from_base62(id);
        self.runtime.block_on(async {
            let id = match parsed {
                Ok(id) => id,
                Err(e) => return ListPage::failed(&format!("bad playlist id: {}", e)),
            };
            match Playlist::get(&self.session, &id).await {
                Ok(list) => self.playlist_page(&list, offset, limit).await,
                Err(e) => ListPage::failed(&e.to_string()),
            }
        })
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
        match self.runtime.block_on(self.search_page(query)) {
            Ok(page) => page,
            Err(e) => ListPage::failed(&e),
        }
    }

    fn play(&self, id: &str) -> Reply {
        let mut r = Reply::new();
        let parsed = SpotifyId::from_base62(id);
        let ok = self.runtime.block_on(async {
            let id = match parsed {
                Ok(id) => id,
                Err(_) => return false,
            };
            if let Some(wire) = self.describe(&id).await {
                self.shared.duration_ms.store(wire.duration_ms, Ordering::SeqCst);
                *self.shared.now.lock().unwrap() = Some(wire);
            }
            // Playing one track from a list: the list is the queue, and this
            // is the cursor into it, so next/previous move through what the
            // person was looking at.
            self.shared.count.store(1, Ordering::SeqCst);
            self.shared.index.store(0, Ordering::SeqCst);
            *self.shared.queue.lock().unwrap() = vec![id];
            self.player.load(id, true, 0);
            true
        });
        r.verdict(ok, "could not load the track");
        r
    }

    fn play_context(&self, kind: &str, context: &str, id: &str) -> Reply {
        let mut r = Reply::new();
        let cursor_id = SpotifyId::from_base62(id);
        let kind = kind.to_string();
        let context = context.to_string();

        let ok = self.runtime.block_on(async {
            let cursor_id = match cursor_id {
                Ok(id) => id,
                Err(_) => return false,
            };

            // Fill the queue with the whole list, then start at `cursor_id`.
            let ids: Vec<SpotifyId> = match kind.as_str() {
                "liked" => {
                    let user = self.account.clone();
                    match likestate_id(&user) {
                        Some(pl) => match Playlist::get(&self.session, &pl).await {
                            Ok(list) => list.tracks().cloned().collect(),
                            Err(_) => vec![cursor_id],
                        },
                        None => vec![cursor_id],
                    }
                }
                "playlist" => match SpotifyId::from_base62(&context) {
                    Ok(pl) => match Playlist::get(&self.session, &pl).await {
                        Ok(list) => list.tracks().cloned().collect(),
                        Err(_) => vec![cursor_id],
                    },
                    Err(_) => vec![cursor_id],
                },
                _ => vec![cursor_id],
            };

            let cursor = ids.iter().position(|x| *x == cursor_id).unwrap_or(0);
            self.set_queue(ids, cursor);
            self.load_cursor().await.is_ok()
        });
        r.verdict(ok, "could not start that list");
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
        // Failing to skip (end of the queue) is not an error the player shows:
        // the status keeps reporting the last track and stays put.
        let _ = self.runtime.block_on(self.skip(true));
    }

    fn previous(&self) {
        let _ = self.runtime.block_on(self.skip(false));
    }

    fn seek(&self, ms: u32) {
        self.player.seek(ms);
        self.shared.position_ms.store(ms as i64, Ordering::SeqCst);
    }

    fn shuffle(&self, on: bool) {
        self.shared.shuffle.store(on, Ordering::SeqCst);
    }

    fn repeat(&self, mode: u32) {
        self.shared.repeat.store(mode as u64, Ordering::SeqCst);
    }

    fn volume(&self, percent: i64) {
        // The player's own keys change its DAC; this is the number the phone
        // and the status page agree on, not a second attenuator.
        self.shared.volume.store(percent.clamp(0, 100), Ordering::SeqCst);
    }

    fn queue(&self) -> Reply {
        let ids: Vec<SpotifyId> = self.shared.queue.lock().unwrap().clone();
        let index = self.shared.index.load(Ordering::SeqCst);
        let mut r = Reply::new();
        r.line(format!("Q\t{}\t{}", index + 1, ids.len()));
        for id in ids.iter() {
            r.line(format!("T\t{}\t\t\t\t", id.to_base62().unwrap_or_default()));
        }
        r.done();
        r
    }

    fn auth(&self) -> Reply {
        if self.account.is_empty() {
            Reply::error("AUTH none")
        } else {
            let mut r = Reply::new();
            r.line(format!("AUTH ok {}", self.account));
            r.done();
            r
        }
    }

    /// Give the sink the player's socket for the life of a PCM channel. Boxed
    /// because the transport is the daemon's choice, not the sink's: a UNIX
    /// socket on a test rig, TCP to a player on the Wi-Fi.
    fn open_pcm(&self, stream: Box<dyn std::io::Write + Send>) {
        *self.shared.pcm.lock().unwrap() = Some(stream);
    }

    fn close_pcm(&self) {
        *self.shared.pcm.lock().unwrap() = None;
    }
}

// Keeps the HashMap import honest: the describe cache this file used to keep is
// gone (one request per row is fast enough and much easier to reason about),
// but a future version that batches artist/album lookups will want it again.
#[allow(dead_code)]
type DescribeCache = HashMap<SpotifyId, String>;
