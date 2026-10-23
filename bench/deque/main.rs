use std::collections::VecDeque;

const N: i64 = 10_000_000;

fn main() {
    let mut d: VecDeque<i64> = VecDeque::new();
    let (mut sum, mut most) = (0i64, 0usize);
    for i in 0..N {
        d.push_back(i);
        d.push_front(i);
        if i % 3 == 2 {
            sum += d.pop_front().unwrap();
            sum += d.pop_back().unwrap();
        }
        if d.len() > most {
            most = d.len();
        }
    }
    for i in 0..d.len() {
        sum += d[i] % 7;
    }
    while let Some(v) = d.pop_front() {
        sum += v;
        if let Some(w) = d.pop_back() {
            sum -= w;
        }
    }
    println!("{} {}", most, sum);
}
