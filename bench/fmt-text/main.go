package main

import (
	"fmt"
	"strings"
)

const N = 2000000

func main() {
	names := []string{"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"}
	var out strings.Builder
	for i := int64(0); i < N; i++ {
		id := i * 2654435761 % 4294967296
		value := float64(i) / 7.0
		fmt.Fprintf(&out, "%08x %-12s %14.3f %+d\n", id, names[i%8], value, i)
	}
	s := out.String()
	sum := int64(0)
	for i := 0; i < len(s); i++ {
		sum += int64(s[i])
	}
	fmt.Println(len(s), sum)
}
