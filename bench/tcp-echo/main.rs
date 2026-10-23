use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

#[tokio::main]
async fn main() {
    let ln = TcpListener::bind("127.0.0.1:9000").await.unwrap();
    loop {
        let (mut c, _) = match ln.accept().await {
            Ok(x) => x,
            Err(_) => continue,
        };
        tokio::spawn(async move {
            let mut buf = [0u8; 4096];
            loop {
                let n = match c.read(&mut buf).await {
                    Ok(0) | Err(_) => return,
                    Ok(n) => n,
                };
                if c.write_all(&buf[..n]).await.is_err() {
                    return;
                }
            }
        });
    }
}
