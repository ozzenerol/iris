use std::fmt::Write;

fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

fn count_keys(v: &toml::Value) -> usize {
    match v {
        toml::Value::Table(t) => t.len() + t.values().map(count_keys).sum::<usize>(),
        toml::Value::Array(a) => a.iter().map(count_keys).sum(),
        _ => 0,
    }
}

// A 2 MB document of tables made in memory, read 20 times into a general
// value, counting the keys.
fn main() {
    let mut doc = String::new();
    let mut seed = 7i64;
    let mut i = 0i64;
    while doc.len() < 2000000 {
        let r = next(&mut seed);
        write!(doc, "[server_{i}]\nname = \"srv-{i}\"\nport = {}\nenabled = true\nweight = {}.5\ntags = [\"a{}\", \"b\", \"c\"]\n\n[server_{i}.limits]\nmax = {r}\nburst = {}\n\n", 8000 + r % 1000, r % 100, r % 3, r % 64).unwrap();
        i += 1;
    }
    let mut total = 0;
    for _ in 0..20 {
        let v: toml::Table = doc.parse().unwrap();
        total += count_keys(&toml::Value::Table(v));
    }
    println!("{}", total);
}
