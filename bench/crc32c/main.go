package main

import (
	"fmt"
	"hash/crc32"
	"strings"
)

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

var words = []string{"alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"}

// The CRC32C of a 64 KiB block, carried on through 16000 rounds (1 GiB).
func main() {
	seed := int64(1)
	var b strings.Builder
	for b.Len() < 65536 {
		b.WriteString(words[next(&seed)%10])
	}
	chunk := []byte(b.String())[:65536]
	table := crc32.MakeTable(crc32.Castagnoli)
	c := uint32(0)
	for i := 0; i < 16000; i++ {
		c = crc32.Update(c, table, chunk)
	}
	fmt.Println(c)
}
