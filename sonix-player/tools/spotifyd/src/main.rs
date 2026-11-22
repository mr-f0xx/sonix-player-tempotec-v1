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

// The wire format is the package's library target (src/lib.rs), so the binary
// and the tests share one compiled copy instead of each growing its own.
use protocol;

use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::sync::Arc;
use std::thread;

use protocol::{ListPage, Reply};

// Discovery is how a player that has never been configured finds this daemon:
// one UDP broadcast, one answer. The numbers must match spotify.h in the
// player, which is the only other end of this conversation.
const DISCOVERY_PORT: u16 = 7790;
const DISCOVERY_MAGIC: u32 = 0x5350_5431; // "SPT1"
const PROTOCOL_VERSION: u32 = 1;

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

#[derive(Clone)]
pub struct Config {
    /// "host:port" to listen on, or a path for a UNIX socket. "0.0.0.0:6600"
    /// is the default: the player is on another machine, so a UNIX socket
    /// alone would be a daemon nobody can reach.
    pub socket: String,
    pub cache: String,
    pub device_name: String,
    pub bitrate: u16,
    pub bits: u16,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            socket: "0.0.0.0:6600".into(),
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
// Send + Sync: one daemon serves several connections at once, each on its own
// thread, so every thread holds an Arc to the same Library. That is only sound
// if the implementation can be shared that way -- without the bound the
// compiler rejects the spawn at the accept loop, which is where this was
// caught.
pub trait Library: Send + Sync {
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
    /// Give the sink the player's socket for the life of a PCM channel. Boxed
    /// because the transport is the daemon's choice, not the sink's: a UNIX
    /// socket on a test rig, TCP to a player on the Wi-Fi.
    fn open_pcm(&self, stream: Box<dyn Write + Send>);
    fn close_pcm(&self);
}

/// One line, one reply. The return value says whether the connection should
/// stay open -- `END` closes it, and so does a client that has gone away.
pub fn dispatch<W: Write>(lib: &dyn Library, line: &str, out: &mut W) -> std::io::Result<bool> {
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
        // The transport commands carry no answer: the caller reads the new
        // state from the next STATUS. Status::encode() and ListPage::encode()
        // both produce a Reply, which is what this match yields, so a bare
        // Reply::new() is already the right empty one.
        "TOGGLE" => {
            lib.toggle();
            Reply::new()
        }
        "RESUME" => {
            lib.resume();
            Reply::new()
        }
        "PAUSE" => {
            lib.pause();
            Reply::new()
        }
        "NEXT" => {
            lib.next();
            Reply::new()
        }
        "PREV" => {
            lib.previous();
            Reply::new()
        }
        "SEEK" => {
            lib.seek(num(0, 0) as u32);
            Reply::new()
        }
        "SHUFFLE" => {
            lib.shuffle(arg(0) != "0");
            Reply::new()
        }
        "REPEAT" => {
            lib.repeat((num(0, 0).clamp(0, 2)) as u32);
            Reply::new()
        }
        "VOLUME" => {
            lib.volume(num(0, 50).clamp(0, 100));
            Reply::new()
        }
        "PING" => Reply::new(),
        "END" => {
            out.write_all(b"END\n")?;
            return Ok(false);
        }
        // PCM is the connection loop's business: it changes what the rest of
        // the connection means, so it never reaches this table.
        other => Reply::error(&format!("unknown command {}", other)),
    };

    // Reply::encode() is the wire form: the lines, each terminated, and
    // exactly one END. There is no as_bytes on it -- the bytes come from this.
    out.write_all(reply.encode().as_bytes())?;
    Ok(true)
}

// ---------------------------------------------------------------------------
// connections
// ---------------------------------------------------------------------------

/// What the connection body needs from a socket, and nothing more. UnixStream
/// and TcpStream disagree about almost everything except this: they can be
/// read, written, and cloned so the reader and the PCM handover can each hold
/// one. A trait rather than a generic parameter with two copies of the loop,
/// because the loop -- and especially the PCM takeover -- is the part most
/// worth having exactly one of.
trait PlayerStream: Read + Write {
    fn duplicate(&self) -> std::io::Result<Self>
    where
        Self: Sized;
}

impl PlayerStream for UnixStream {
    fn duplicate(&self) -> std::io::Result<Self> {
        self.try_clone()
    }
}

impl PlayerStream for std::net::TcpStream {
    fn duplicate(&self) -> std::io::Result<Self> {
        self.try_clone()
    }
}

// Send and 'static are part of the bound because the PCM handover boxes a
// duplicate of the socket and hands it to librespot's own thread
// (Library::open_pcm takes Box<dyn Write + Send>).
fn serve<S: PlayerStream + Send + 'static>(
    lib: Arc<dyn Library>,
    mut stream: S,
    bits: u16,
) -> std::io::Result<()> {
    let reader = stream.duplicate()?;
    let mut reader = BufReader::new(reader);
    let mut line = String::new();

    loop {
        line.clear();
        if reader.read_line(&mut line)? == 0 {
            return Ok(()); // the player closed the connection
        }

        // PCM takes the connection over: the header, then nothing but samples
        // until the player closes it.
        if line.trim_end() == "PCM" {
            stream.write_all(&protocol::PCM_MAGIC)?;
            stream.write_all(format!("44100 2 {}\n", bits).as_bytes())?;
            lib.open_pcm(Box::new(stream.duplicate()?));
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

fn serve_unix(lib: Arc<dyn Library>, stream: UnixStream, bits: u16) -> std::io::Result<()> {
    serve(lib, stream, bits)
}

fn serve_tcp(lib: Arc<dyn Library>, stream: std::net::TcpStream, bits: u16) -> std::io::Result<()> {
    // A player that has gone quiet must not sit in a socket buffer for the
    // length of a track, and the PCM frames are already sized by the decoder.
    // Latency matters here in a way it does not for a browse page.
    stream.set_nodelay(true).ok();
    serve(lib, stream, bits)
}

// ---------------------------------------------------------------------------
// startup
// ---------------------------------------------------------------------------

/// How the configured address is read: "host:port" means TCP, anything else is
/// a UNIX socket path. The player uses the same rule (address_is_network() in
/// spotify.c), which is what lets one configuration file describe both an
/// on-device test rig and a daemon on the Wi-Fi.
fn split_host_port(address: &str) -> Option<(&str, u16)> {
    let (host, port) = address.rsplit_once(':')?;
    let port: u16 = port.parse().ok()?;
    if port == 0 {
        return None;
    }
    Some((host, port))
}

/// Answers discovery probes from players that have no address configured.
///
/// A person who installs the daemon on a laptop and turns on the Spotify tile
/// should not have to type an IP address into a d-pad. The player broadcasts a
/// probe on UDP 7790 and this thread answers with the port, so the player can
/// dial back to whoever answered. The probe and the answer are both two
/// uint32s: the magic, then either the protocol version (probe) or the port
/// (answer). Nothing is answered that does not carry the magic, so an
/// unrelated broadcast on the same port is not mistaken for a player.
fn advertise(port: u16) {
    use std::net::UdpSocket;
    let socket = match UdpSocket::bind(("0.0.0.0", DISCOVERY_PORT)) {
        Ok(s) => s,
        Err(e) => {
            // Not fatal: a firewall that eats UDP 7790 must not stop the
            // daemon working for somebody who types the address in.
            eprintln!(
                "spotifyd: automatic discovery is off (cannot bind UDP {}: {}). \
                 Point the player at this computer's address and port {} by hand: \
                 Account > long-press Find on this network.",
                DISCOVERY_PORT, e, port
            );
            return;
        }
    };
    eprintln!("spotifyd: answering player discovery on UDP {}", DISCOVERY_PORT);
    let mut buf = [0u8; 8];
    loop {
        let (n, from) = match socket.recv_from(&mut buf) {
            Ok(v) => v,
            Err(_) => continue,
        };
        if n < buf.len() {
            continue;
        }
        let magic = u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]);
        if magic != DISCOVERY_MAGIC {
            continue;
        }
        let p = u32::from_be_bytes([buf[4], buf[5], buf[6], buf[7]]);
        if p != PROTOCOL_VERSION {
            continue; // a player from a future version: let it fall back
        }
        // Magic echoed, then the port we listen on, both big-endian.
        let mut answer = [0u8; 8];
        answer[..4].copy_from_slice(&DISCOVERY_MAGIC.to_be_bytes());
        answer[4..].copy_from_slice(&(port as u32).to_be_bytes());
        let _ = socket.send_to(&answer, from);
    }
}

fn serve_unix_loop(lib: Arc<dyn Library>, listener: UnixListener, bits: u16) {
    for stream in listener.incoming() {
        let stream = match stream {
            Ok(s) => s,
            Err(e) => {
                eprintln!("spotifyd: accept failed: {}", e);
                continue;
            }
        };
        let lib = Arc::clone(&lib);
        thread::spawn(move || {
            if let Err(e) = serve_unix(lib, stream, bits) {
                eprintln!("spotifyd: connection ended: {}", e);
            }
        });
    }
}

fn serve_tcp_loop(lib: Arc<dyn Library>, listener: std::net::TcpListener, bits: u16) {
    for stream in listener.incoming() {
        let stream = match stream {
            Ok(s) => s,
            Err(e) => {
                eprintln!("spotifyd: accept failed: {}", e);
                continue;
            }
        };
        let lib = Arc::clone(&lib);
        thread::spawn(move || {
            if let Err(e) = serve_tcp(lib, stream, bits) {
                eprintln!("spotifyd: connection ended: {}", e);
            }
        });
    }
}

/// Where spotifyd.conf lives: next to the binary first, so a checkout can carry
/// its own, then the two system places.
fn find_config() -> Option<String> {
    let beside = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("spotifyd.conf")))
        .filter(|p| p.exists())
        .map(|p| p.to_string_lossy().into_owned());
    beside
        .or_else(|| {
            ["/etc/spotifyd.conf", "spotifyd.conf"]
                .iter()
                .find(|p| std::path::Path::new(p).exists())
                .map(|p| p.to_string())
        })
}

fn main() {
    // The player is explicit about what it wants; a daemon that dies on a bad
    // environment gives the person on the sofa nothing to act on.
    let cfg = match find_config() {
        Some(path) => {
            eprintln!("spotifyd: reading {}", path);
            read_config(&path)
        }
        None => {
            eprintln!("spotifyd: no spotifyd.conf found, using defaults");
            Config::default()
        }
    };

    let lib = match librespot_glue::connect(&cfg) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("spotifyd: cannot start the Spotify session: {}", e);
            std::process::exit(1);
        }
    };

    if let Some((host, port)) = split_host_port(&cfg.socket) {
        let addr = if host.is_empty() { "0.0.0.0" } else { host };
        let listener = match std::net::TcpListener::bind((addr, port)) {
            Ok(l) => l,
            Err(e) => {
                eprintln!("spotifyd: cannot listen on {}:{}: {}", addr, port, e);
                std::process::exit(1);
            }
        };
        eprintln!("spotifyd: listening on {}:{}", addr, port);
        // Discovery runs for the life of the process and is allowed to fail:
        // a firewall that eats UDP 7790 must not stop the player working for
        // somebody who typed the address in.
        thread::spawn(move || advertise(port));
        serve_tcp_loop(lib, listener, cfg.bits);
    } else {
        let path = cfg.socket.clone();
        let listener = match UnixListener::bind(&path) {
            Ok(l) => l,
            Err(e) => {
                eprintln!("spotifyd: cannot bind {}: {}", path, e);
                std::process::exit(1);
            }
        };
        eprintln!("spotifyd: listening on {}", path);
        serve_unix_loop(lib, listener, cfg.bits);
    }
}
