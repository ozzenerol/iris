use sqlx::mysql::MySqlPoolOptions;

#[derive(sqlx::FromRow)]
struct Item {
    id: i32,
    name: String,
    email: String,
    score: f64,
    qty: i32,
}

// Against a MySQL server at $IRIS_BENCH_MYSQL: 1000 rows inserted in a
// transaction, 20000 rows read one at a time by id with a prepared
// statement, then the whole table read 100 times, all on one connection.
#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<(), sqlx::Error> {
    let address = std::env::var("IRIS_BENCH_MYSQL").unwrap_or("mysql://iris:secret@127.0.0.1:3306/iris_test".into());
    let address = address.split('?').next().unwrap().to_string();
    let db = MySqlPoolOptions::new().max_connections(1).connect(&address).await?;
    sqlx::raw_sql("DROP TABLE IF EXISTS items; CREATE TABLE items (id int PRIMARY KEY, name varchar(50) NOT NULL, email varchar(100) NOT NULL, score double NOT NULL, qty int NOT NULL)")
        .execute(&db)
        .await?;
    let mut tx = db.begin().await?;
    for i in 1..1001 {
        sqlx::query("INSERT INTO items (id, name, email, score, qty) VALUES (?, ?, ?, ?, ?)")
            .bind(i)
            .bind(format!("item {}", i))
            .bind(format!("buyer{}@example.com", i))
            .bind(i as f64 * 0.5)
            .bind(i % 17)
            .execute(&mut *tx)
            .await?;
    }
    tx.commit().await?;
    let mut score = 0.0f64;
    let mut chars = 0i64;
    for i in 0..20000i64 {
        let it: Item = sqlx::query_as("SELECT id, name, email, score, qty FROM items WHERE id = ?")
            .bind(i * 7919 % 1000 + 1)
            .fetch_one(&db)
            .await?;
        score += it.score;
        chars += (it.name.len() + it.email.len()) as i64 + it.qty as i64;
    }
    let mut ids = 0i64;
    for _ in 0..100 {
        let items: Vec<Item> = sqlx::query_as("SELECT id, name, email, score, qty FROM items WHERE qty >= ?")
            .bind(0)
            .fetch_all(&db)
            .await?;
        for it in items {
            ids += it.id as i64;
        }
    }
    println!("{} {} {}", score, chars, ids);
    Ok(())
}
