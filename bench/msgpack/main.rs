// The same orders with serde and rmp-serde, as maps with the field names.

use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Default)]
struct Item {
    name: String,
    qty: i32,
    price: f64,
}

#[derive(Serialize, Deserialize, Default)]
struct Order {
    id: i64,
    customer: String,
    items: Vec<Item>,
    tags: Vec<String>,
    codes: Vec<i32>,
    paid: bool,
}

fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

fn encode(o: &Order) -> Vec<u8> {
    rmp_serde::to_vec_named(o).unwrap()
}

fn decode(b: &[u8]) -> Order {
    rmp_serde::from_slice(b).unwrap()
}

// 100,000 orders decoded and encoded again, 20 times.
fn main() {
    let mut seed = 1;
    let mut stream = Vec::new();
    let mut ends = Vec::new();
    for i in 0..100000i64 {
        let mut o = Order { id: i * 7919, paid: i % 3 == 0, ..Default::default() };
        o.customer = format!("customer {}", next(&mut seed) % 10000);
        for _ in 0..(1 + next(&mut seed) % 4) {
            let name = format!("item {}", next(&mut seed) % 500);
            let qty = (1 + next(&mut seed) % 9) as i32;
            let price = (next(&mut seed) % 10000) as f64 / 100.0;
            o.items.push(Item { name, qty, price });
        }
        for j in 0..(next(&mut seed) % 3) {
            o.tags.push(format!("tag{}", j));
        }
        for _ in 0..(next(&mut seed) % 6) {
            o.codes.push((next(&mut seed) % 100000) as i32);
        }
        stream.extend_from_slice(&encode(&o));
        ends.push(stream.len());
    }
    let mut total: i64 = 0;
    for _ in 0..20 {
        let mut start = 0;
        for &end in &ends {
            let o = decode(&stream[start..end]);
            total += o.id + o.customer.len() as i64 + o.tags.len() as i64;
            for it in &o.items {
                total += it.qty as i64 + it.name.len() as i64 + it.price as i64;
            }
            for c in &o.codes {
                total += *c as i64;
            }
            let b = encode(&o);
            total += b.len() as i64;
            start = end;
        }
    }
    println!("{}", total);
}
