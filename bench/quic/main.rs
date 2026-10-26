// A QUIC server and client in one program, over loopback: 200 connections,
// each with a handshake and a request on a stream, then 512 MiB through one
// stream, with quinn and rustls (aws-lc-rs): X25519MLKEM768 and
// TLS13_AES_256_GCM_SHA384, rustls's first choices, as Iris uses.
use quinn::{ClientConfig, Endpoint, ServerConfig};
use rustls::pki_types::pem::PemObject;
use rustls::pki_types::{CertificateDer, PrivateKeyDer};
use std::path::PathBuf;
use std::sync::Arc;

async fn serve_stream(mut send: quinn::SendStream, mut recv: quinn::RecvStream) {
    let mut buf = vec![0u8; 64 << 10];
    let mut total = 0usize;
    while let Ok(Some(n)) = recv.read(&mut buf).await {
        total += n;
    }
    let reply = if total == 5 { "ok".to_string() } else { total.to_string() };
    let _ = send.write_all(reply.as_bytes()).await;
    let _ = send.finish();
    let _ = send.stopped().await;
}

async fn serve_conn(c: quinn::Connection) {
    while let Ok((send, recv)) = c.accept_bi().await {
        tokio::spawn(serve_stream(send, recv));
    }
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let _ = rustls::crypto::aws_lc_rs::default_provider().install_default();
    let mut data = PathBuf::from(std::env::current_exe().unwrap().parent().unwrap());
    data.push("../../tests/data/tls");
    let certs: Vec<CertificateDer> = CertificateDer::pem_file_iter(data.join("server.pem"))
        .unwrap()
        .map(|c| c.unwrap())
        .collect();
    let key = PrivateKeyDer::from_pem_file(data.join("server.key")).unwrap();
    let mut roots = rustls::RootCertStore::empty();
    for c in CertificateDer::pem_file_iter(data.join("ca.pem")).unwrap() {
        roots.add(c.unwrap()).unwrap();
    }

    let mut stls = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(certs, key)
        .unwrap();
    stls.alpn_protocols = vec![b"bench".to_vec()];
    let scfg = ServerConfig::with_crypto(Arc::new(
        quinn::crypto::rustls::QuicServerConfig::try_from(stls).unwrap(),
    ));
    let server = Endpoint::server(scfg, "127.0.0.1:0".parse().unwrap()).unwrap();
    let addr = server.local_addr().unwrap();
    tokio::spawn(async move {
        while let Some(inc) = server.accept().await {
            tokio::spawn(async move {
                if let Ok(c) = inc.await {
                    serve_conn(c).await;
                }
            });
        }
    });

    let mut ctls = rustls::ClientConfig::builder()
        .with_root_certificates(roots)
        .with_no_client_auth();
    ctls.alpn_protocols = vec![b"bench".to_vec()];
    let ccfg = ClientConfig::new(Arc::new(
        quinn::crypto::rustls::QuicClientConfig::try_from(ctls).unwrap(),
    ));
    let mut client = Endpoint::client("127.0.0.1:0".parse().unwrap()).unwrap();
    client.set_default_client_config(ccfg);

    let mut good = 0;
    for _ in 0..200 {
        let c = client.connect(addr, "localhost").unwrap().await.unwrap();
        let (mut send, mut recv) = c.open_bi().await.unwrap();
        send.write_all(b"hello").await.unwrap();
        send.finish().unwrap();
        let reply = recv.read_to_end(1 << 20).await.unwrap();
        if reply == b"ok" {
            good += 1;
        }
        c.close(0u32.into(), b"");
    }

    let c = client.connect(addr, "localhost").unwrap().await.unwrap();
    let (mut send, mut recv) = c.open_bi().await.unwrap();
    let block: Vec<u8> = (0..1 << 20).map(|i| (i * 7) as u8).collect();
    for _ in 0..512 {
        send.write_all(&block).await.unwrap();
    }
    send.finish().unwrap();
    let counted = recv.read_to_end(1 << 20).await.unwrap();
    c.close(0u32.into(), b"");
    println!("connections {}, bytes {}", good, String::from_utf8_lossy(&counted));
}
