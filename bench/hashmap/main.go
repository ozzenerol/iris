package main

import "fmt"

const N = 10000000

func main() {
	m := map[int64]int64{}
	var x uint64 = 88172645463325252
	for i := int64(0); i < N; i++ {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		m[int64(x%50000000)] = i
	}
	var hits, sum int64
	for i := int64(0); i < N; i++ {
		if v, ok := m[i*5]; ok {
			hits++
			sum += v
		}
	}
	fmt.Println(len(m), hits, sum)
}
