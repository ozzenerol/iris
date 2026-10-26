package main

import (
	"encoding/hex"
	"fmt"
	"strings"
)

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

var words = []string{"alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"}

// A 64 KiB block to hex and back, 2000 times.
func main() {
	seed := int64(1)
	var b strings.Builder
	for b.Len() < 65536 {
		b.WriteString(words[next(&seed)%10])
	}
	chunk := []byte(b.String())[:65536]
	total := 0
	for i := 0; i < 2000; i++ {
		text := hex.EncodeToString(chunk)
		back, err := hex.DecodeString(text)
		if err != nil {
			panic(err)
		}
		total += len(text) + int(back[i%len(back)])
	}
	fmt.Println(total)
}
