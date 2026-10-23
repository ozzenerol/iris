use std::cmp::Reverse;
use std::collections::BinaryHeap;

const N: usize = 2_000_000;

fn main() {
    let mut h = BinaryHeap::new();
    let mut x: u64 = 88172645463325252;
    for _ in 0..N {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        h.push(Reverse((x % 1_000_000_000) as i64));
    }
    let first = h.peek().unwrap().0;
    let mut prev = first;
    let mut sum: i64 = 0;
    while let Some(Reverse(v)) = h.pop() {
        if v < prev {
            println!("not in order");
            return;
        }
        prev = v;
        sum += v;
    }
    println!("{} {} {}", first, prev, sum);
}
