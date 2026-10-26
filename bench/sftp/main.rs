// An SFTP server and client in one program, over loopback, through one
// SSH connection: 1000 small files written, stat'ed, read back and
// removed, then a 256 MiB file uploaded and downloaded. russh-sftp's
// client and server, the server keeping the files in a temporary folder
// (russh-sftp leaves the file system to the program). The multi-threaded
// runtime, as the Go server and the Iris one run on other cores.
// mlkem768x25519-sha256 and AES-256-GCM, as the Iris and Go versions use.

use std::borrow::Cow;
use std::collections::HashMap;
use std::fs::{self, File, OpenOptions};
use std::os::unix::fs::FileExt;
use std::path::PathBuf;
use std::sync::Arc;

use russh::keys::{Algorithm, PrivateKey, PrivateKeyWithHashAlg, PublicKey, PublicKeyOrCertificate};
use russh::server::{Auth, Msg, Session, VerifiedPublicKey};
use russh::{client, server, Channel, ChannelId, Preferred};
use russh_sftp::protocol::{Attrs, Data, FileAttributes, Handle, OpenFlags, Status, StatusCode};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio::sync::Mutex;

fn preferred() -> Preferred {
    Preferred {
        kex: Cow::Owned(vec![russh::kex::MLKEM768X25519_SHA256]),
        key: Cow::Owned(vec![Algorithm::Ed25519]),
        cipher: Cow::Owned(vec![russh::cipher::AES_256_GCM]),
        ..Preferred::default()
    }
}

struct Files {
    root: PathBuf,
    open: HashMap<String, File>,
    next: u64,
}

fn status(id: u32, code: StatusCode) -> Status {
    Status { id, status_code: code, error_message: String::new(), language_tag: "en".into() }
}

fn failed(e: std::io::Error) -> StatusCode {
    if e.kind() == std::io::ErrorKind::NotFound {
        StatusCode::NoSuchFile
    } else if e.kind() == std::io::ErrorKind::PermissionDenied {
        StatusCode::PermissionDenied
    } else {
        StatusCode::Failure
    }
}

impl Files {
    fn path(&self, p: &str) -> PathBuf {
        self.root.join(p.trim_start_matches('/'))
    }
}

impl russh_sftp::server::Handler for Files {
    type Error = StatusCode;

    fn unimplemented(&self) -> Self::Error {
        StatusCode::OpUnsupported
    }

    async fn open(&mut self, id: u32, name: String, flags: OpenFlags, _: FileAttributes) -> Result<Handle, Self::Error> {
        let f = OpenOptions::new()
            .read(flags.contains(OpenFlags::READ))
            .write(flags.contains(OpenFlags::WRITE))
            .create(flags.contains(OpenFlags::CREATE))
            .truncate(flags.contains(OpenFlags::TRUNCATE))
            .open(self.path(&name))
            .map_err(failed)?;
        self.next += 1;
        let handle = self.next.to_string();
        self.open.insert(handle.clone(), f);
        Ok(Handle { id, handle })
    }

    async fn close(&mut self, id: u32, handle: String) -> Result<Status, Self::Error> {
        self.open.remove(&handle);
        Ok(status(id, StatusCode::Ok))
    }

    async fn read(&mut self, id: u32, handle: String, offset: u64, len: u32) -> Result<Data, Self::Error> {
        let f = self.open.get(&handle).ok_or(StatusCode::Failure)?;
        let mut data = vec![0; len.min(256 * 1024) as usize];
        let n = f.read_at(&mut data, offset).map_err(failed)?;
        if n == 0 {
            return Err(StatusCode::Eof);
        }
        data.truncate(n);
        Ok(Data { id, data })
    }

    async fn write(&mut self, id: u32, handle: String, offset: u64, data: Vec<u8>) -> Result<Status, Self::Error> {
        let f = self.open.get(&handle).ok_or(StatusCode::Failure)?;
        f.write_all_at(&data, offset).map_err(failed)?;
        Ok(status(id, StatusCode::Ok))
    }

    async fn stat(&mut self, id: u32, path: String) -> Result<Attrs, Self::Error> {
        let m = fs::metadata(self.path(&path)).map_err(failed)?;
        Ok(Attrs { id, attrs: FileAttributes::from(&m) })
    }

    async fn lstat(&mut self, id: u32, path: String) -> Result<Attrs, Self::Error> {
        let m = fs::symlink_metadata(self.path(&path)).map_err(failed)?;
        Ok(Attrs { id, attrs: FileAttributes::from(&m) })
    }

    async fn fstat(&mut self, id: u32, handle: String) -> Result<Attrs, Self::Error> {
        let f = self.open.get(&handle).ok_or(StatusCode::Failure)?;
        let m = f.metadata().map_err(failed)?;
        Ok(Attrs { id, attrs: FileAttributes::from(&m) })
    }

    async fn remove(&mut self, id: u32, path: String) -> Result<Status, Self::Error> {
        fs::remove_file(self.path(&path)).map_err(failed)?;
        Ok(status(id, StatusCode::Ok))
    }
}

struct Server {
    user: PublicKey,
    root: PathBuf,
    channels: Arc<Mutex<HashMap<ChannelId, Channel<Msg>>>>,
}

impl server::Handler for Server {
    type Error = russh::Error;

    async fn auth_publickey(&mut self, _: &str, key: VerifiedPublicKey<'_>) -> Result<Auth, Self::Error> {
        if key.public_key.key_data() == self.user.key_data() {
            Ok(Auth::Accept)
        } else {
            Ok(Auth::reject())
        }
    }

    async fn channel_open_session(
        &mut self,
        channel: Channel<Msg>,
        reply: server::ChannelOpenHandle,
        _: &mut Session,
    ) -> Result<(), Self::Error> {
        self.channels.lock().await.insert(channel.id(), channel);
        reply.accept().await;
        Ok(())
    }

    async fn subsystem_request(&mut self, id: ChannelId, name: &str, session: &mut Session) -> Result<(), Self::Error> {
        let channel = self.channels.lock().await.remove(&id);
        match channel {
            Some(channel) if name == "sftp" => {
                session.channel_success(id)?;
                let files = Files { root: self.root.clone(), open: HashMap::new(), next: 0 };
                russh_sftp::server::run(channel.into_stream(), files).await;
            }
            _ => session.channel_failure(id)?,
        }
        Ok(())
    }
}

struct Client {
    host: PublicKey,
}

impl client::Handler for Client {
    type Error = russh::Error;

    async fn check_server_key(&mut self, key: &PublicKeyOrCertificate) -> Result<bool, Self::Error> {
        Ok(matches!(key, PublicKeyOrCertificate::PublicKey { key, .. } if key.key_data() == self.host.key_data()))
    }
}

#[tokio::main]
async fn main() {
    let root = std::env::temp_dir().join(format!("sftp-bench-{}", std::process::id()));
    fs::create_dir_all(&root).unwrap();
    let host = PrivateKey::random(&mut rand::rng(), Algorithm::Ed25519).unwrap();
    let user = Arc::new(PrivateKey::random(&mut rand::rng(), Algorithm::Ed25519).unwrap());
    let host_public = host.public_key().clone();
    let user_public = user.public_key().clone();
    let server_config = Arc::new(server::Config { keys: vec![host], preferred: preferred(), ..Default::default() });
    let ln = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = ln.local_addr().unwrap();
    let served = root.clone();
    tokio::spawn(async move {
        loop {
            let (stream, _) = ln.accept().await.unwrap();
            stream.set_nodelay(true).unwrap();
            let handler = Server { user: user_public.clone(), root: served.clone(), channels: Default::default() };
            let config = server_config.clone();
            tokio::spawn(async move {
                if let Ok(s) = server::run_stream(config, stream, handler).await {
                    let _ = s.await;
                }
            });
        }
    });
    let client_config = Arc::new(client::Config { preferred: preferred(), nodelay: true, ..Default::default() });
    let mut c = client::connect(client_config, addr, Client { host: host_public }).await.unwrap();
    let ok = c.authenticate_publickey("bench", PrivateKeyWithHashAlg::new(user, None)).await.unwrap();
    assert!(ok.success());
    let ch = c.channel_open_session().await.unwrap();
    ch.request_subsystem(true, "sftp").await.unwrap();
    let s = russh_sftp::client::SftpSession::new(ch.into_stream()).await.unwrap();

    let small: Vec<u8> = (0..1024usize).map(|i| (i * 7 & 255) as u8).collect();
    let mut files = 0;
    for i in 0..1000 {
        let name = format!("f{i}");
        let mut f = s.create(name.as_str()).await.unwrap();
        f.write_all(&small).await.unwrap();
        f.shutdown().await.unwrap();
        let info = s.metadata(name.as_str()).await.unwrap();
        let back = s.read(name.as_str()).await.unwrap();
        if info.size == Some(small.len() as u64) && back == small {
            files += 1;
        }
        s.remove_file(name.as_str()).await.unwrap();
    }

    let block: Vec<u8> = (0..1usize << 20).map(|i| (i * 7 & 255) as u8).collect();
    let big = block.repeat(256);
    let mut f = s.create("big").await.unwrap();
    f.write_all(&big).await.unwrap();
    f.shutdown().await.unwrap();
    let mut r = s.open("big").await.unwrap();
    let mut got = Vec::with_capacity(big.len());
    r.read_to_end(&mut got).await.unwrap();
    println!("files {files}, bytes {}, same {}", got.len(), got == big);
    fs::remove_dir_all(&root).unwrap();
}
