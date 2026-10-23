use axum::{routing::post, Json, Router};
use serde::{Deserialize, Serialize};

#[derive(Deserialize)]
struct Item {
    #[allow(dead_code)]
    name: String,
    qty: i64,
    price: f64,
}

#[derive(Deserialize)]
struct Order {
    id: i64,
    #[allow(dead_code)]
    customer: String,
    items: Vec<Item>,
    #[allow(dead_code)]
    tags: Vec<String>,
}

#[derive(Serialize)]
struct Total {
    id: i64,
    count: usize,
    total: f64,
}

async fn order(Json(o): Json<Order>) -> Json<Total> {
    let total = o.items.iter().map(|it| it.qty as f64 * it.price).sum();
    Json(Total { id: o.id, count: o.items.len(), total })
}

#[tokio::main]
async fn main() {
    let app = Router::new().route("/order", post(order));
    let ln = tokio::net::TcpListener::bind("127.0.0.1:9000").await.unwrap();
    axum::serve(ln, app).await.unwrap();
}
