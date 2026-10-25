package main

import (
	"fmt"
	"strings"

	"gopkg.in/yaml.v3"
)

const n = 50000

// A YAML list of 50000 records (nested maps, a flow list, quoted and plain
// text, numbers, booleans) parsed into values and read back, 3 times.
func main() {
	var b strings.Builder
	for i := 0; i < n; i++ {
		fmt.Fprintf(&b, "- id: %d\n  name: \"user %d\"\n  active: %v\n  score: %d.5\n", i, i, i%3 == 0, i%100)
		fmt.Fprintf(&b, "  tags: [net, \"tag %d\", edge]\n  address:\n    city: Iasi\n    zip: \"7000%d\"\n", i%7, i%10)
	}
	text := b.String()
	total := 0
	for k := 0; k < 3; k++ {
		var v []map[string]any
		if err := yaml.Unmarshal([]byte(text), &v); err != nil {
			panic(err)
		}
		for _, r := range v {
			total += r["id"].(int) + len(r["name"].(string)) + len(r["tags"].([]any))
			total += len(r["address"].(map[string]any)["city"].(string)) + int(r["score"].(float64))
			if r["active"].(bool) {
				total++
			}
		}
	}
	fmt.Println(len(text), total)
}
