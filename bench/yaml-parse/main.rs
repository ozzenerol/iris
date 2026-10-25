use serde_yaml::Value;
use std::fmt::Write;

const N: usize = 50000;

// A YAML list of 50000 records (nested maps, a flow list, quoted and plain
// text, numbers, booleans) parsed into values and read back, 3 times.
fn main() {
    let mut text = String::new();
    for i in 0..N {
        write!(text, "- id: {}\n  name: \"user {}\"\n  active: {}\n  score: {}.5\n", i, i, i % 3 == 0, i % 100).unwrap();
        write!(text, "  tags: [net, \"tag {}\", edge]\n  address:\n    city: Iasi\n    zip: \"7000{}\"\n", i % 7, i % 10).unwrap();
    }
    let mut total = 0i64;
    for _ in 0..3 {
        let v: Value = serde_yaml::from_str(&text).unwrap();
        for r in v.as_sequence().unwrap() {
            total += r["id"].as_i64().unwrap() + r["name"].as_str().unwrap().len() as i64;
            total += r["tags"].as_sequence().unwrap().len() as i64;
            total += r["address"]["city"].as_str().unwrap().len() as i64 + r["score"].as_f64().unwrap() as i64;
            if r["active"].as_bool().unwrap() {
                total += 1;
            }
        }
    }
    println!("{} {}", text.len(), total);
}
