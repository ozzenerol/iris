package main

import (
	"cmp"
	"fmt"
	"slices"
)

const N = 5000000

type Order struct {
	key   int64
	id    int64
	total float64
}

func main() {
	xs := make([]Order, N)
	var x uint64 = 88172645463325252
	for i := range xs {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		xs[i] = Order{key: int64(x >> 1), id: int64(i), total: float64(i % 1000)}
	}
	slices.SortFunc(xs, func(a, b Order) int { return cmp.Compare(a.key, b.key) })
	for i := 1; i < N; i++ {
		if xs[i-1].key > xs[i].key {
			fmt.Println("not sorted")
			return
		}
	}
	fmt.Println(xs[0].key, xs[N/2].key, xs[N-1].key)
}
