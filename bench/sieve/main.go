package main

import "fmt"

const N = 100000000

func main() {
	composite := make([]bool, N+1)
	count := 0
	for i := 2; i <= N; i++ {
		if !composite[i] {
			count++
			for j := i * i; j <= N; j += i {
				composite[j] = true
			}
		}
	}
	fmt.Println(count)
}
