package main

import "fmt"

type Node struct{ left, right *Node }

func make_(depth int) *Node {
	if depth > 0 {
		return &Node{make_(depth - 1), make_(depth - 1)}
	}
	return &Node{}
}

func (n *Node) check() int {
	if n.left == nil {
		return 1
	}
	return 1 + n.left.check() + n.right.check()
}

const maxDepth = 21

func main() {
	fmt.Printf("stretch tree of depth %d\t check: %d\n", maxDepth+1, make_(maxDepth+1).check())
	long := make_(maxDepth)
	for d := 4; d <= maxDepth; d += 2 {
		iterations := 1 << (maxDepth - d + 4)
		sum := 0
		for i := 0; i < iterations; i++ {
			sum += make_(d).check()
		}
		fmt.Printf("%d\t trees of depth %d\t check: %d\n", iterations, d, sum)
	}
	fmt.Printf("long lived tree of depth %d\t check: %d\n", maxDepth, long.check())
}
