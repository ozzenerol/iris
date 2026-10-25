fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

const WORDS: [&str; 10] = ["alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"];

// The CRC32C of a 64 KiB block, carried on through 16000 rounds (1 GiB).
fn main() {
    let mut seed = 1;
    let mut text = String::new();
    while text.len() < 65536 {
        text.push_str(WORDS[(next(&mut seed) % 10) as usize]);
    }
    let chunk = &text.as_bytes()[..65536];
    let mut c = 0u32;
    for _ in 0..16000 {
        c = crc32c::crc32c_append(c, chunk);
    }
    println!("{}", c);
}
