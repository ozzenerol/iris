package main

import (
	"fmt"
	"slices"
	"sort"
)

const N = 1000000
const SCANS = 1000

type entry struct {
	key, val int64
}

// Go has no sorted map: the entries go into a slice, sorted once, and are
// found by binary search.
func main() {
	keys := make([]int64, N)
	es := make([]entry, 0, N)
	var x uint64 = 88172645463325252
	for i := 0; i < N; i++ {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		k := int64(x >> 24)
		keys[i] = k
		es = append(es, entry{k, int64(i)})
	}
	// stable, so for a key set twice the later value is last; keep that one
	slices.SortStableFunc(es, func(a, b entry) int {
		if a.key < b.key {
			return -1
		}
		if a.key > b.key {
			return 1
		}
		return 0
	})
	m := es[:0]
	for _, e := range es {
		if len(m) > 0 && m[len(m)-1].key == e.key {
			m[len(m)-1] = e
		} else {
			m = append(m, e)
		}
	}
	find := func(k int64) int { return sort.Search(len(m), func(i int) bool { return m[i].key >= k }) }
	var found, sum int64
	for i := 0; i < N; i++ {
		var k int64
		if i%2 == 0 {
			k = keys[(i*7)%N]
		} else {
			x ^= x << 13
			x ^= x >> 7
			x ^= x << 17
			k = int64(x >> 24)
		}
		j := find(k)
		if j < len(m) && m[j].key == k {
			found++
			sum += m[j].val
		}
	}
	var seen, total int64
	for s := 0; s < SCANS; s++ {
		from := keys[(s*997)%N]
		for j := find(from); j < len(m) && m[j].key < from+1073741824; j++ {
			seen++
			total += m[j].val
		}
	}
	fmt.Println(len(m), found, sum, seen, total)
}
