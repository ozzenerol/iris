package main

import (
	"fmt"
	"sync"
	"sync/atomic"
)

const ROUNDS = 2000000
const KEYS = 100000

func main() {
	var counts sync.Map
	var wg sync.WaitGroup
	for t := uint64(0); t < 4; t++ {
		wg.Add(1)
		go func(id uint64) {
			defer wg.Done()
			seen := int64(0)
			x := id*2654435761 + 1
			for i := 0; i < ROUNDS; i++ {
				x ^= x << 13
				x ^= x >> 7
				x ^= x << 17
				k := int64(x % KEYS)
				if i%5 == 0 {
					v, ok := counts.Load(k)
					if !ok {
						v, _ = counts.LoadOrStore(k, new(atomic.Int64))
					}
					v.(*atomic.Int64).Add(1)
				} else if v, ok := counts.Load(k); ok {
					seen += v.(*atomic.Int64).Load()
				}
			}
			_ = seen
		}(t)
	}
	wg.Wait()
	total := int64(0)
	counts.Range(func(k, v any) bool {
		total += v.(*atomic.Int64).Load()
		return true
	})
	fmt.Println(total)
}
