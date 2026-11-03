use dashmap::DashMap;
use std::sync::Arc;
use std::thread;

const ROUNDS: i64 = 2_000_000;
const KEYS: u64 = 100_000;

fn main() {
    let counts: Arc<DashMap<i64, i64>> = Arc::new(DashMap::new());
    let handles: Vec<_> = (0..4u64)
        .map(|id| {
            let counts = counts.clone();
            thread::spawn(move || {
                let mut seen = 0i64;
                let mut x: u64 = id.wrapping_mul(2654435761).wrapping_add(1);
                for i in 0..ROUNDS {
                    x ^= x << 13;
                    x ^= x >> 7;
                    x ^= x << 17;
                    let k = (x % KEYS) as i64;
                    if i % 5 == 0 {
                        *counts.entry(k).or_insert(0) += 1;
                    } else {
                        seen += counts.get(&k).map(|v| *v).unwrap_or(0);
                    }
                }
                std::hint::black_box(seen);
            })
        })
        .collect();
    for h in handles {
        h.join().unwrap();
    }
    let total: i64 = counts.iter().map(|e| *e.value()).sum();
    println!("{}", total);
}
