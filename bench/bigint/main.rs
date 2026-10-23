use num_bigint::BigUint;

fn main() {
    let mut f = BigUint::from(1u32);
    for i in 2u32..=20000 {
        f *= i;
    }
    let digits = f.to_string();
    let m = (BigUint::from(1u32) << 2048usize) - BigUint::from(159u32);
    let e = BigUint::from(65537u32);
    let mut check = 0u64;
    for i in 0u32..300 {
        let b = BigUint::from(i + 3).pow(300);
        let r = b.modpow(&e, &m);
        check += r.bits();
    }
    println!("{} {} {}", digits.len(), &digits[..12], check);
}
