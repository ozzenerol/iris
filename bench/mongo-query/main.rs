use futures::TryStreamExt;
use mongodb::bson::doc;
use mongodb::options::{ClientOptions, IndexOptions};
use mongodb::{Client, IndexModel};
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize)]
struct Item {
    n: i64,
    name: String,
    email: String,
    score: f64,
    qty: i64,
}

// Against a MongoDB server at $IRIS_BENCH_MONGO: 1000 documents inserted one
// at a time, 20000 documents found one at a time by n, then the whole
// collection read 100 times, all on one connection.
#[tokio::main(flavor = "current_thread")]
async fn main() -> mongodb::error::Result<()> {
    let address = std::env::var("IRIS_BENCH_MONGO").unwrap_or("mongodb://iris:secret@127.0.0.1:27017/iris_test?authSource=admin".into());
    let mut opts = ClientOptions::parse(&address).await?;
    opts.max_pool_size = Some(1);
    opts.direct_connection = Some(true);
    let client = Client::with_options(opts)?;
    let items = client.database("iris_test").collection::<Item>("items");
    items.drop().await?;
    items
        .create_index(IndexModel::builder().keys(doc! {"n": 1}).options(IndexOptions::builder().unique(true).build()).build())
        .await?;
    for i in 1..1001i64 {
        let it = Item { n: i, name: format!("item {}", i), email: format!("buyer{}@example.com", i), score: i as f64 * 0.5, qty: i % 17 };
        items.insert_one(&it).await?;
    }
    let mut score = 0.0f64;
    let mut chars = 0i64;
    for i in 0..20000i64 {
        let it = items.find_one(doc! {"n": i * 7919 % 1000 + 1}).await?.unwrap();
        score += it.score;
        chars += (it.name.len() + it.email.len()) as i64 + it.qty;
    }
    let mut ids = 0i64;
    for _ in 0..100 {
        let found: Vec<Item> = items.find(doc! {"qty": {"$gte": 0}}).await?.try_collect().await?;
        for it in found {
            ids += it.n;
        }
    }
    println!("{} {} {}", score, chars, ids);
    Ok(())
}
