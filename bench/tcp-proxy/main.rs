use tokio::net::{TcpListener, TcpStream};

#[tokio::main]
async fn main() {
    let ln = TcpListener::bind("127.0.0.1:9000").await.unwrap();
    loop {
        let (mut c, _) = match ln.accept().await {
            Ok(x) => x,
            Err(_) => continue,
        };
        tokio::spawn(async move {
            let _ = c.set_nodelay(true);
            let mut b = match TcpStream::connect("127.0.0.1:9001").await {
                Ok(b) => b,
                Err(_) => return,
            };
            let _ = b.set_nodelay(true);
            let _ = tokio::io::copy_bidirectional(&mut c, &mut b).await;
        });
    }
}
