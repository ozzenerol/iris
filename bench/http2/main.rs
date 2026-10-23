use axum::{routing::get, Router};

#[tokio::main]
async fn main() {
    let app = Router::new().route("/", get(|| async { "hello, world\n" }));
    let ln = tokio::net::TcpListener::bind("127.0.0.1:9000").await.unwrap();
    axum::serve(ln, app).await.unwrap();
}
