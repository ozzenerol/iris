package main

import (
	"fmt"
	"strings"

	"github.com/BurntSushi/toml"
)

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

func countKeys(v any) int {
	n := 0
	switch t := v.(type) {
	case map[string]any:
		n += len(t)
		for _, x := range t {
			n += countKeys(x)
		}
	case []any:
		for _, x := range t {
			n += countKeys(x)
		}
	case []map[string]any:
		for _, x := range t {
			n += countKeys(x)
		}
	}
	return n
}

// A 2 MB document of tables made in memory, read 20 times into a general
// value, counting the keys.
func main() {
	var doc strings.Builder
	seed := int64(7)
	for i := 0; doc.Len() < 2000000; i++ {
		r := next(&seed)
		fmt.Fprintf(&doc, "[server_%d]\nname = \"srv-%d\"\nport = %d\nenabled = true\nweight = %d.5\ntags = [\"a%d\", \"b\", \"c\"]\n\n[server_%d.limits]\nmax = %d\nburst = %d\n\n", i, i, 8000+r%1000, r%100, r%3, i, r, r%64)
	}
	text := doc.String()
	total := 0
	for k := 0; k < 20; k++ {
		var v map[string]any
		if _, err := toml.Decode(text, &v); err != nil {
			panic(err)
		}
		total += countKeys(v)
	}
	fmt.Println(total)
}
