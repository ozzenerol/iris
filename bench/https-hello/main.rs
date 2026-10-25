use axum::{routing::get, Router};
use axum_server::tls_rustls::RustlsConfig;

// cert.pem and key.pem are in the folder it runs in
#[tokio::main]
async fn main() {
    let app = Router::new().route("/", get(|| async { "hello, world\n" }));
    let config = RustlsConfig::from_pem_file("cert.pem", "key.pem").await.unwrap();
    let addr = "127.0.0.1:9000".parse().unwrap();
    axum_server::bind_rustls(addr, config).serve(app.into_make_service()).await.unwrap();
}
