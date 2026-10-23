use std::collections::BTreeMap;

const N: usize = 1_000_000;
const SCANS: usize = 1000;

fn main() {
    let mut m = BTreeMap::new();
    let mut keys = vec![0i64; N];
    let mut x: u64 = 88172645463325252;
    for i in 0..N {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        let k = (x >> 24) as i64;
        keys[i] = k;
        m.insert(k, i as i64);
    }
    let (mut found, mut sum) = (0i64, 0i64);
    for i in 0..N {
        let k = if i % 2 == 0 {
            keys[(i * 7) % N]
        } else {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            (x >> 24) as i64
        };
        if let Some(v) = m.get(&k) {
            found += 1;
            sum += v;
        }
    }
    let (mut seen, mut total) = (0i64, 0i64);
    for s in 0..SCANS {
        let from = keys[(s * 997) % N];
        for (_, v) in m.range(from..from + 1073741824) {
            seen += 1;
            total += v;
        }
    }
    println!("{} {} {} {} {}", m.len(), found, sum, seen, total);
}
