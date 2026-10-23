const REPEAT: usize = 400_000;

fn main() {
    let piece = "Köln, Москва и 東京 – ok 😀 ";
    let mut text = String::with_capacity(piece.len() * REPEAT);
    for _ in 0..REPEAT {
        text.push_str(piece);
    }
    let bytes = text.as_bytes();
    let mut total: i64 = 0;
    for _ in 0..3 {
        total += text.chars().count() as i64;
        let sum: i64 = text.chars().map(|c| c as i64).sum();
        total += sum % 1000003;
        if std::str::from_utf8(bytes).is_ok() {
            total += 1;
        }
    }
    println!("{} {}", text.len(), total);
}
