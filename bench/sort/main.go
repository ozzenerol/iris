package main

import "fmt"

const N = 10000000

func quicksort(a []int64, lo, hi int) {
	for hi-lo > 16 {
		pivot := a[lo+(hi-lo)/2]
		i, j := lo, hi
		for i <= j {
			for a[i] < pivot {
				i++
			}
			for a[j] > pivot {
				j--
			}
			if i <= j {
				a[i], a[j] = a[j], a[i]
				i++
				j--
			}
		}
		if j-lo < hi-i {
			quicksort(a, lo, j)
			lo = i
		} else {
			quicksort(a, i, hi)
			hi = j
		}
	}
	for i := lo + 1; i <= hi; i++ {
		v := a[i]
		j := i - 1
		for j >= lo && a[j] > v {
			a[j+1] = a[j]
			j--
		}
		a[j+1] = v
	}
}

func main() {
	a := make([]int64, N)
	var x uint64 = 88172645463325252
	for i := range a {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		a[i] = int64(x >> 1)
	}
	quicksort(a, 0, N-1)
	for i := 1; i < N; i++ {
		if a[i-1] > a[i] {
			fmt.Println("not sorted")
			return
		}
	}
	fmt.Println(a[0], a[N/2], a[N-1])
}
