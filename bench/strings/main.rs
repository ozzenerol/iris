use std::fmt::Write;

const N: usize = 5_000_000;

fn main() {
    let mut s = String::new();
    for i in 0..N {
        write!(s, "item-{},", i).unwrap();
    }
    let found = s.matches("99,").count();
    let parts: Vec<&str> = s.split(',').collect();
    let total: usize = parts.iter().map(|p| p.len()).sum();
    let up = s[..1_000_000].to_uppercase();
    let rep = s.replace("item", "x");
    println!("{} {} {} {} {} {}", s.len(), found, parts.len(), total, up.len(), rep.len());
}
