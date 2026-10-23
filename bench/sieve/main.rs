const N: usize = 100_000_000;

fn main() {
    let mut composite = vec![false; N + 1];
    let mut count = 0;
    for i in 2..=N {
        if !composite[i] {
            count += 1;
            let mut j = i * i;
            while j <= N {
                composite[j] = true;
                j += i;
            }
        }
    }
    println!("{}", count);
}
