use ring::aead::{Aad, LessSafeKey, Nonce, UnboundKey, AES_256_GCM};

fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

const WORDS: [&str; 10] = ["alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"];

// A 64 KiB block sealed with AES-256-GCM and opened again 2000 times,
// each round with its own key and nonce.
fn main() {
    let mut seed = 1;
    let mut text = String::new();
    while text.len() < 65536 {
        text.push_str(WORDS[(next(&mut seed) % 10) as usize]);
    }
    let chunk = &text.as_bytes()[..65536];
    let mut c = crc32fast::Hasher::new();
    for i in 0..2000 {
        let at = i % 997;
        let key = LessSafeKey::new(UnboundKey::new(&AES_256_GCM, &chunk[at..at + 32]).unwrap());
        let nonce = || Nonce::try_assume_unique_for_key(&chunk[at + 32..at + 44]).unwrap();
        let mut sealed = chunk.to_vec();
        key.seal_in_place_append_tag(nonce(), Aad::empty(), &mut sealed).unwrap();
        c.update(&sealed[sealed.len() - 16..]);
        let back = key.open_in_place(nonce(), Aad::empty(), &mut sealed).unwrap();
        c.update(&back[i..i + 1]);
    }
    println!("{}", c.finalize());
}
