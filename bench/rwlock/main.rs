use std::sync::{Arc, RwLock};
use std::thread;

const ROUNDS: i64 = 1_000_000;

fn main() {
    let lock = Arc::new(RwLock::new(0i64));
    let handles: Vec<_> = (0..4)
        .map(|_| {
            let lock = lock.clone();
            thread::spawn(move || {
                let mut seen = 0i64;
                for i in 0..ROUNDS {
                    if i % 10 == 0 {
                        *lock.write().unwrap() += 1;
                    } else {
                        seen += *lock.read().unwrap();
                    }
                }
                std::hint::black_box(seen);
            })
        })
        .collect();
    for h in handles {
        h.join().unwrap();
    }
    println!("{}", *lock.read().unwrap());
}
