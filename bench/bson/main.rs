use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize)]
struct Order {
    _id: bson::oid::ObjectId,
    customer: String,
    email: String,
    total: f64,
    items: i64,
    paid: bool,
    tags: Vec<String>,
    created: bson::DateTime,
}

// Encodes 500000 orders to BSON and decodes each one back, and prints sums
// over what came back.
fn main() {
    let mut id = [0u8; 12];
    for i in 0..12 {
        id[i] = (i * 17) as u8;
    }
    let mut o = Order {
        _id: bson::oid::ObjectId::from_bytes(id),
        customer: "Ada Lovelace".into(),
        email: "ada@example.com".into(),
        total: 0.0,
        items: 0,
        paid: true,
        tags: vec!["express".into(), "gift".into(), "fragile".into()],
        created: bson::DateTime::from_millis(0),
    };
    let mut total = 0.0f64;
    let mut sum = 0i64;
    for i in 0..500000i64 {
        o.total = i as f64 * 0.5;
        o.items = i % 17;
        o.created = bson::DateTime::from_millis(1792000000000 + i);
        let buf = bson::to_vec(&o).unwrap();
        let back: Order = bson::from_slice(&buf).unwrap();
        total += back.total;
        sum += back.items + back.customer.len() as i64 + back.tags.len() as i64 + back._id.bytes()[11] as i64
            + back.created.timestamp_millis() % 7 + buf.len() as i64;
    }
    println!("{} {}", total, sum);
}
