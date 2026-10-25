use aes::cipher::{KeyIvInit, StreamCipher};

type Aes256Ctr = ctr::Ctr128BE<aes::Aes256>;

fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

const WORDS: [&str; 10] = ["alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"];

// A 64 KiB block encrypted with AES-256 in counter mode 4000 times over,
// each round on the result of the one before with its own key.
fn main() {
    let mut seed = 1;
    let mut text = String::new();
    while text.len() < 65536 {
        text.push_str(WORDS[(next(&mut seed) % 10) as usize]);
    }
    let chunk = &text.as_bytes()[..65536];
    let mut data = chunk.to_vec();
    for i in 0..4000 {
        let at = i % 997;
        let mut c = Aes256Ctr::new(chunk[at..at + 32].into(), chunk[at + 32..at + 48].into());
        c.apply_keystream(&mut data);
    }
    println!("{}", crc32fast::hash(&data));
}
