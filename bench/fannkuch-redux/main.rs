const N: usize = 11;

fn main() {
    let mut perm1 = [0usize; 16];
    let mut perm = [0usize; 16];
    let mut count = [0usize; 16];
    for i in 0..N {
        perm1[i] = i;
    }
    let (mut max_flips, mut checksum, mut perm_count) = (0i64, 0i64, 0i64);
    let mut r = N;
    loop {
        while r != 1 {
            count[r - 1] = r;
            r -= 1;
        }
        perm = perm1;
        let mut flips = 0i64;
        let mut k = perm[0];
        while k != 0 {
            let (mut i, mut j) = (0, k);
            while i < j {
                perm.swap(i, j);
                i += 1;
                j -= 1;
            }
            flips += 1;
            k = perm[0];
        }
        if flips > max_flips {
            max_flips = flips;
        }
        checksum += if perm_count % 2 == 0 { flips } else { -flips };
        loop {
            if r == N {
                println!("{}\nPfannkuchen({}) = {}", checksum, N, max_flips);
                return;
            }
            let p0 = perm1[0];
            for i in 0..r {
                perm1[i] = perm1[i + 1];
            }
            perm1[r] = p0;
            count[r] -= 1;
            if count[r] > 0 {
                break;
            }
            r += 1;
        }
        perm_count += 1;
    }
}
