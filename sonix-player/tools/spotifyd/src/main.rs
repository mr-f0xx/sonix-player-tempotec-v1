// spotifyd -- the daemon half of Spotify on the TempoTec V1.
//
// The player (sonix-player/src/system/streaming/spotify.c) is the client. This
// program owns the librespot session, browses the library, keeps the queue, and
// writes decoded audio to the player as raw PCM on the same socket.
//
// Read in this order:
//
//   PROTOCOL.md          the contract between the two halves
//   src/protocol.rs      that contract in code, with tests, no librespot
//   src/librespot_glue.rs the librespot calls -- not compiled in this repo
//   README.md            building it on a PC and pointing the player at it
//
// What this file does is the part that does not change when librespot's API
// moves: read the config, bind the socket, dispatch a line to a handler, write
// the reply. The handlers themselves live in the glue.

mod librespot_glue;
mod protocol;

use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::sync::Arc;
use std::thread;

use protocol::{ListPage, Reply};

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

#[derive(Clone)]
pub struct Config {
    pub socket: String,
    pub cache: String,
    pub device_name: String,
    pub bitrate: u16,
    pub bits: u16,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            socket: "/var/run/sonix-spotify.sock".into(),
            cache: "/usr/data/librespot-cache".into(),
            device_name: "Sonix Player".into(),
            bitrate: 320,
            bits: 16,
        }
    }
}

/// spotifyd.conf is key=value, one per line, `#` starts a comment. A key this
/// build does not know is reported and ignored rather than refused: a newer
/// player may write one, and failing to start over an unknown line would leave
/// the V1 with a tile that leads nowhere.
pub fn read_config(path: &str) -> Config {
    let mut cfg = Config::default();
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(_) => return cfg,
    };
    for line in text.lines() {
        let line = line.split('#').next().unwrap_or("").trim();
        let (key, value) = match line.split_once('=') {
            Some(kv) => (kv.0.trim(), kv.1.trim()),
            None => continue,
        };
        match key {
            "socket" => cfg.socket = value.to_string(),
            "cache" => cfg.cache = value.to_string(),
            "device_name" => cfg.device_name = value.to_string(),
            "bitrate" => cfg.bitrate = value.parse().unwrap_or(cfg.bitrate),
            "bits" => cfg.bits = value.parse().unwrap_or(cfg.bits),
            _ => eprintln!("spotifyd: unknown config key `{}`, ignored", key),
        }
    }
    cfg
}

// ---------------------------------------------------------------------------
// dispatch
// ---------------------------------------------------------------------------

/// How the reply to a command is produced. The protocol layer asks these
/// questions in words; the glue answers them with librespot. Keeping the seam
/// here is what lets `cargo test` cover the wire format with no session, no
/// network and no credentials.
pub trait Library {
    fn status(&self) -> protocol::Status;
    fn liked(&self, offset: usize, limit: usize) -> ListPage;
    fn playlist(&self, id: &str, offset: usize, limit: usize) -> ListPage;
    fn playlists(&self) -> Reply;
    fn search(&self, query: &str) -> ListPage;
    fn play(&self, id: &str) -> Reply;
    fn play_context(&self, kind: &str, context: &str, id: &str) -> Reply;
    fn toggle(&self);
    fn resume(&self);
    fn pause(&self);
    fn next(&self);
    fn previous(&self);
    fn seek(&self, ms: u32);
    fn shuffle(&self, on: bool);
    fn repeat(&self, mode: u32);
    fn volume(&self, percent: i64);
    fn queue(&self) -> Reply;
    fn auth(&self) -> Reply;
    /// Give the sink the player's socket for the life of a PCM channel.
    fn open_pcm(&self, stream: UnixStream);
    fn close_pcm(&self);
}

/// One line, one reply. The return value says whether the connection should
/// stay open -- `END` closes it, and so does a client that has gone away.
pub fn dispatch(lib: &dyn Library, line: &str, out: &mut UnixStream) -> std::io::Result<bool> {
    let text = line.trim_end_matches(['\n', '\r']);
    if text.is_empty() {
        return Ok(true);
    }
    let mut parts = text.split_whitespace();
    let word = match parts.next() {
        Some(w) => w,
        None => return Ok(true),
    };
    let args: Vec<&str> = parts.collect();
    let arg = |i: usize| args.get(i).copied().unwrap_or("");
    let num = |i: usize, fallback: i64| args.get(i).and_then(|s| s.parse().ok()).unwrap_or(fallback);

    let reply: Reply = match word {
        "STATUS" => lib.status().encode(),
        "LIKED" => lib.liked(num(0, 0) as usize, num(1, 50) as usize).encode(),
        "PLAYLIST" => {
            if arg(0).is_empty() {
                Reply::error("PLAYLIST needs a playlist id")
            } else {
                lib.playlist(arg(0), num(1, 0) as usize, num(2, 50) as usize).encode()
            }
        }
        "PLAYLISTS" => lib.playlists(),
        "SEARCH" => {
            if args.is_empty() {
                Reply::error("SEARCH needs a query")
            } else {
                lib.search(&args.join(" ")).encode()
            }
        }
        "QUEUE" => lib.queue(),
        "AUTH" => lib.auth(),
        "PLAY" => {
            if arg(0).is_empty() {
                Reply::error("PLAY needs a track id")
            } else {
                lib.play(arg(0))
            }
        }
        "PLAYCTX" => {
            if args.len() < 3 {
                Reply::error("PLAYCTX needs <kind> <context> <track>")
            } else {
                lib.play_context(arg(0), arg(1), arg(2))
            }
        }
        "TOGGLE" => {
            lib.toggle();
            Reply::new().encode()
        }
        "RESUME" => {
            lib.resume();
            Reply::new().encode()
        }
        "PAUSE" => {
            lib.pause();
            Reply::new().encode()
        }
        "NEXT" => {
            lib.next();
            Reply::new().encode()
        }
        "PREV" => {
            lib.previous();
            Reply::new().encode()
        }
        "SEEK" => {
            lib.seek(num(0, 0) as u32);
            Reply::new().encode()
        }
        "SHUFFLE" => {
            lib.shuffle(arg(0) != "0");
            Reply::new().encode()
        }
        "REPEAT" => {
            lib.repeat((num(0, 0).clamp(0, 2)) as u32);
            Reply::new().encode()
        }
        "VOLUME" => {
            lib.volume(num(0, 50).clamp(0, 100));
            Reply::new().encode()
        }
        "PING" => Reply::new().encode(),
        "END" => {
            out.write_all(b"END\n")?;
            return Ok(false);
        }
        // PCM is the connection loop's business: it changes what the rest of
        // the connection means, so it never reaches this table.
        other => Reply::error(&format!("unknown command {}", other)).encode(),
    };

    out.write_all(reply.as_bytes())?;
    Ok(true)
}

// ---------------------------------------------------------------------------
// connections
// ---------------------------------------------------------------------------

fn serve(lib: Arc<dyn Library>, mut stream: UnixStream, bits: u16) -> std::io::Result<()> {
    let reader = stream.try_clone()?;
    let mut reader = BufReader::new(reader);
    let mut line = String::new();

    loop {
        line.clear();
        if reader.read_line(&mut line)? == 0 {
            return Ok(()); // the player closed the connection
        }

        // PCM takes the connection over: the reply header, then nothing but
        // samples until the player closes it.
        if line.trim_end() == "PCM" {
            stream.write_all(&protocol::PCM_MAGIC)?;
            stream.write_all(format!("44100 2 {}\n", bits).as_bytes())?;
            lib.open_pcm(stream.try_clone()?);
            let mut sink = [0u8; 4096];
            loop {
                match reader.read(&mut sink) {
                    Ok(0) | Err(_) => break,
                    // The player sends nothing on this channel. Anything that
                    // arrives is dropped, not answered: answering would put a
                    // command line in the middle of the samples.
                    Ok(_) => continue,
                }
            }
            lib.close_pcm();
            return Ok(());
        }

        if !dispatch(lib.as_ref(), &line, &mut stream)? {
            return Ok(());
        }
    }
}

fn main() {
    let mut args = std::env::args().skip(1);
    let mut conf_path = "/usr/data/spotifyd.conf".to_string();
    while let Some(a) = args.next() {
        match a.as_str() {
            "--config" => {
                if let Some(p) = args.next() {
                    conf_path = p;
                }
            }
            "--help" | "-h" => {
                println!(
                    "spotifyd --config <path>\n\n\
                     The daemon half of Spotify on the TempoTec V1.\n\
                     PROTOCOL.md is the socket contract; README.md is the setup."
                );
                return;
            }
            "--version" | "-V" => {
                println!("spotifyd {}", env!("CARGO_PKG_VERSION"));
                return;
            }
            other => eprintln!("spotifyd: unknown argument `{}`, ignored", other),
        }
    }

    let cfg = read_config(&conf_path);
    eprintln!(
        "spotifyd: socket={} cache={} device={} bitrate={} bits={}",
        cfg.socket, cfg.cache, cfg.device_name, cfg.bitrate, cfg.bits
    );

    // A tokio runtime for the whole process: every librespot call is async and
    // each connection's handlers block_on it. One thread per connection, one
    // library session, one player.
    let lib: Arc<dyn Library> = match librespot_glue::connect(&cfg) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("spotifyd: could not sign in: {}", e);
            eprintln!(
                "spotifyd: log in once on this computer:\n  \
                 librespot --cache {} --cache-tool login --username <you>",
                cfg.cache
            );
            std::process::exit(1);
        }
    };

    let _ = std::fs::remove_file(&cfg.socket);
    let listener = match UnixListener::bind(&cfg.socket) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("spotifyd: cannot bind {}: {}", cfg.socket, e);
            std::process::exit(1);
        }
    };
    eprintln!("spotifyd: listening on {}", cfg.socket);

    let bits = cfg.bits;
    for conn in listener.incoming() {
        match conn {
            Ok(stream) => {
                let lib = lib.clone();
                thread::spawn(move || {
                    if let Err(e) = serve(lib, stream, bits) {
                        eprintln!("spotifyd: a connection ended: {}", e);
                    }
                });
            }
            Err(e) => eprintln!("spotifyd: accept failed: {}", e),
        }
    }
}
