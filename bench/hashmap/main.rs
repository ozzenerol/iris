use std::collections::HashMap;

const N: i64 = 10_000_000;

fn main() {
    let mut m: HashMap<i64, i64> = HashMap::new();
    let mut x: u64 = 88172645463325252;
    for i in 0..N {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        m.insert((x % 50_000_000) as i64, i);
    }
    let (mut hits, mut sum) = (0i64, 0i64);
    for i in 0..N {
        if let Some(v) = m.get(&(i * 5)) {
            hits += 1;
            sum += v;
        }
    }
    println!("{} {} {}", m.len(), hits, sum);
}
