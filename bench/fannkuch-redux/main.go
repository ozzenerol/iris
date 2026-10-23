package main

import "fmt"

const N = 11

func main() {
	var perm1, perm, count [16]int
	for i := 0; i < N; i++ {
		perm1[i] = i
	}
	maxFlips, checksum, permCount := 0, 0, 0
	r := N
	for {
		for ; r != 1; r-- {
			count[r-1] = r
		}
		perm = perm1
		flips := 0
		for k := perm[0]; k != 0; k = perm[0] {
			for i, j := 0, k; i < j; i, j = i+1, j-1 {
				perm[i], perm[j] = perm[j], perm[i]
			}
			flips++
		}
		if flips > maxFlips {
			maxFlips = flips
		}
		if permCount%2 == 0 {
			checksum += flips
		} else {
			checksum -= flips
		}
		for {
			if r == N {
				fmt.Printf("%d\nPfannkuchen(%d) = %d\n", checksum, N, maxFlips)
				return
			}
			p0 := perm1[0]
			for i := 0; i < r; i++ {
				perm1[i] = perm1[i+1]
			}
			perm1[r] = p0
			count[r]--
			if count[r] > 0 {
				break
			}
			r++
		}
		permCount++
	}
}
