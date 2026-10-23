package main

import (
	"fmt"
	"sync"
)

const ROUNDS = 1000000

func main() {
	var lock sync.RWMutex
	counter := int64(0)
	var wg sync.WaitGroup
	for t := 0; t < 4; t++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			seen := int64(0)
			for i := 0; i < ROUNDS; i++ {
				if i%10 == 0 {
					lock.Lock()
					counter++
					lock.Unlock()
				} else {
					lock.RLock()
					seen += counter
					lock.RUnlock()
				}
			}
			_ = seen
		}()
	}
	wg.Wait()
	fmt.Println(counter)
}
