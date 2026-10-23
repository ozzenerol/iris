const N: usize = 5_000_000;

#[derive(Clone, Copy)]
struct Order {
    key: i64,
    #[allow(dead_code)]
    id: i64,
    #[allow(dead_code)]
    total: f64,
}

fn main() {
    let mut xs = Vec::with_capacity(N);
    let mut x: u64 = 88172645463325252;
    for i in 0..N {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        xs.push(Order { key: (x >> 1) as i64, id: i as i64, total: (i % 1000) as f64 });
    }
    xs.sort_unstable_by_key(|o| o.key);
    for i in 1..N {
        if xs[i - 1].key > xs[i].key {
            println!("not sorted");
            return;
        }
    }
    println!("{} {} {}", xs[0].key, xs[N / 2].key, xs[N - 1].key);
}
