// An SSH server and client in one program, over loopback: 100 logins with
// an Ed25519 key, each running a command, then 512 MiB through one
// channel. mlkem768x25519-sha256 and AES-256-GCM, as the Iris and Go
// versions use. russh with its default aws-lc-rs crypto.

use std::borrow::Cow;
use std::sync::Arc;

use russh::keys::{Algorithm, PrivateKey, PrivateKeyWithHashAlg, PublicKey, PublicKeyOrCertificate};
use russh::server::{Auth, Msg, Session, VerifiedPublicKey};
use russh::{client, server, Channel, ChannelId, ChannelMsg, Preferred};
use tokio::net::TcpListener;

fn preferred() -> Preferred {
    Preferred {
        kex: Cow::Owned(vec![russh::kex::MLKEM768X25519_SHA256]),
        key: Cow::Owned(vec![Algorithm::Ed25519]),
        cipher: Cow::Owned(vec![russh::cipher::AES_256_GCM]),
        ..Preferred::default()
    }
}

struct Server {
    user: PublicKey,
    counting: bool,
    total: u64,
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
        _: Channel<Msg>,
        reply: server::ChannelOpenHandle,
        _: &mut Session,
    ) -> Result<(), Self::Error> {
        reply.accept().await;
        Ok(())
    }

    async fn exec_request(&mut self, channel: ChannelId, data: &[u8], session: &mut Session) -> Result<(), Self::Error> {
        session.channel_success(channel)?;
        if data == b"count" {
            self.counting = true;
            return Ok(());
        }
        session.data(channel, &b"ok"[..])?;
        session.exit_status_request(channel, 0)?;
        session.eof(channel)?;
        session.close(channel)?;
        Ok(())
    }

    async fn data(&mut self, _: ChannelId, data: &[u8], _: &mut Session) -> Result<(), Self::Error> {
        self.total += data.len() as u64;
        Ok(())
    }

    async fn channel_eof(&mut self, channel: ChannelId, session: &mut Session) -> Result<(), Self::Error> {
        if self.counting {
            session.data(channel, self.total.to_string().into_bytes())?;
            session.exit_status_request(channel, 0)?;
            session.eof(channel)?;
            session.close(channel)?;
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

async fn output(channel: &mut Channel<client::Msg>) -> Vec<u8> {
    let mut out = Vec::new();
    while let Some(msg) = channel.wait().await {
        if let ChannelMsg::Data { data } = msg {
            out.extend_from_slice(&data);
        }
    }
    out
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let host = PrivateKey::random(&mut rand::rng(), Algorithm::Ed25519).unwrap();
    let user = Arc::new(PrivateKey::random(&mut rand::rng(), Algorithm::Ed25519).unwrap());
    let host_public = host.public_key().clone();
    let user_public = user.public_key().clone();
    let server_config = Arc::new(server::Config {
        keys: vec![host],
        preferred: preferred(),
        auth_rejection_time: std::time::Duration::from_secs(0),
        auth_rejection_time_initial: Some(std::time::Duration::from_secs(0)),
        ..Default::default()
    });
    let ln = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = ln.local_addr().unwrap();
    tokio::spawn(async move {
        loop {
            let (stream, _) = ln.accept().await.unwrap();
            // as Iris and Go do; Nagle's algorithm costs each login 80 ms
            stream.set_nodelay(true).unwrap();
            let handler = Server { user: user_public.clone(), counting: false, total: 0 };
            let config = server_config.clone();
            tokio::spawn(async move {
                if let Ok(s) = server::run_stream(config, stream, handler).await {
                    let _ = s.await;
                }
            });
        }
    });
    let client_config = Arc::new(client::Config { preferred: preferred(), nodelay: true, ..Default::default() });

    let mut logins = 0;
    for _ in 0..100 {
        let mut c = client::connect(client_config.clone(), addr, Client { host: host_public.clone() }).await.unwrap();
        let ok = c.authenticate_publickey("bench", PrivateKeyWithHashAlg::new(user.clone(), None)).await.unwrap();
        assert!(ok.success());
        let mut ch = c.channel_open_session().await.unwrap();
        ch.exec(true, "hello").await.unwrap();
        if output(&mut ch).await == b"ok" {
            logins += 1;
        }
        c.disconnect(russh::Disconnect::ByApplication, "", "").await.unwrap();
    }

    let mut c = client::connect(client_config, addr, Client { host: host_public }).await.unwrap();
    let ok = c.authenticate_publickey("bench", PrivateKeyWithHashAlg::new(user, None)).await.unwrap();
    assert!(ok.success());
    let mut ch = c.channel_open_session().await.unwrap();
    ch.exec(true, "count").await.unwrap();
    let block: Vec<u8> = (0..1usize << 20).map(|i| (i * 7 & 255) as u8).collect();
    for _ in 0..512 {
        ch.data_bytes(block.clone()).await.unwrap();
    }
    ch.eof().await.unwrap();
    let counted = output(&mut ch).await;
    println!("logins {}, bytes {}", logins, String::from_utf8_lossy(&counted));
}
