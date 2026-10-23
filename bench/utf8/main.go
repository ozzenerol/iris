package main

import (
	"fmt"
	"strings"
	"unicode/utf8"
)

const REPEAT = 400000

func main() {
	piece := "Köln, Москва и 東京 – ok 😀 "
	var b strings.Builder
	b.Grow(len(piece) * REPEAT)
	for i := 0; i < REPEAT; i++ {
		b.WriteString(piece)
	}
	text := b.String()
	total := int64(0)
	for k := 0; k < 3; k++ {
		total += int64(utf8.RuneCountInString(text))
		sum := int64(0)
		for _, r := range text {
			sum += int64(r)
		}
		total += sum % 1000003
		if utf8.ValidString(text) {
			total++
		}
	}
	fmt.Println(len(text), total)
}
