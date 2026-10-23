use std::fmt::Write;

const N: i64 = 2_000_000;

fn main() {
    let names = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"];
    let mut out = String::new();
    for i in 0..N {
        let id = i * 2654435761 % 4294967296;
        let value = i as f64 / 7.0;
        writeln!(out, "{:08x} {:<12} {:>14.3} {:+}", id, names[(i % 8) as usize], value, i).unwrap();
    }
    let sum: i64 = out.bytes().map(|b| b as i64).sum();
    println!("{} {}", out.len(), sum);
}
