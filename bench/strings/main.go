package main

import (
	"fmt"
	"strconv"
	"strings"
)

const N = 5000000

func main() {
	var b strings.Builder
	for i := 0; i < N; i++ {
		b.WriteString("item-")
		b.WriteString(strconv.Itoa(i))
		b.WriteByte(',')
	}
	s := b.String()
	found := strings.Count(s, "99,")
	parts := strings.Split(s, ",")
	total := 0
	for _, p := range parts {
		total += len(p)
	}
	up := strings.ToUpper(s[:1000000])
	rep := strings.ReplaceAll(s, "item", "x")
	fmt.Println(len(s), found, len(parts), total, len(up), len(rep))
}
