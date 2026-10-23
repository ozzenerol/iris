const N: usize = 10_000_000;

fn quicksort(a: &mut [i64], mut lo: isize, mut hi: isize) {
    while hi - lo > 16 {
        let pivot = a[(lo + (hi - lo) / 2) as usize];
        let (mut i, mut j) = (lo, hi);
        while i <= j {
            while a[i as usize] < pivot {
                i += 1;
            }
            while a[j as usize] > pivot {
                j -= 1;
            }
            if i <= j {
                a.swap(i as usize, j as usize);
                i += 1;
                j -= 1;
            }
        }
        if j - lo < hi - i {
            quicksort(a, lo, j);
            lo = i;
        } else {
            quicksort(a, i, hi);
            hi = j;
        }
    }
    for i in lo + 1..hi + 1 {
        let v = a[i as usize];
        let mut j = i - 1;
        while j >= lo && a[j as usize] > v {
            a[(j + 1) as usize] = a[j as usize];
            j -= 1;
        }
        a[(j + 1) as usize] = v;
    }
}

fn main() {
    let mut a = vec![0i64; N];
    let mut x: u64 = 88172645463325252;
    for v in a.iter_mut() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        *v = (x >> 1) as i64;
    }
    quicksort(&mut a, 0, N as isize - 1);
    for i in 1..N {
        if a[i - 1] > a[i] {
            println!("not sorted");
            return;
        }
    }
    println!("{} {} {}", a[0], a[N / 2], a[N - 1]);
}
