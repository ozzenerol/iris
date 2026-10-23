package main

import (
	"container/heap"
	"fmt"
)

const N = 2000000

type IntHeap []int64

func (h IntHeap) Len() int           { return len(h) }
func (h IntHeap) Less(i, j int) bool { return h[i] < h[j] }
func (h IntHeap) Swap(i, j int)      { h[i], h[j] = h[j], h[i] }
func (h *IntHeap) Push(x any)        { *h = append(*h, x.(int64)) }
func (h *IntHeap) Pop() any {
	old := *h
	n := len(old)
	v := old[n-1]
	*h = old[:n-1]
	return v
}

func main() {
	h := &IntHeap{}
	var x uint64 = 88172645463325252
	for i := 0; i < N; i++ {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		heap.Push(h, int64(x%1000000000))
	}
	first := (*h)[0]
	prev := first
	var sum int64
	for h.Len() > 0 {
		v := heap.Pop(h).(int64)
		if v < prev {
			fmt.Println("not in order")
			return
		}
		prev = v
		sum += v
	}
	fmt.Println(first, prev, sum)
}
