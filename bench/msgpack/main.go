// The same orders with vmihailenco/msgpack, as maps with the field names.
package main

import (
	"bytes"
	"fmt"

	"github.com/vmihailenco/msgpack/v5"
)

type Item struct {
	Name  string  `msgpack:"name"`
	Qty   int32   `msgpack:"qty"`
	Price float64 `msgpack:"price"`
}

type Order struct {
	ID       int64    `msgpack:"id"`
	Customer string   `msgpack:"customer"`
	Items    []Item   `msgpack:"items"`
	Tags     []string `msgpack:"tags"`
	Codes    []int32  `msgpack:"codes"`
	Paid     bool     `msgpack:"paid"`
}

var buf bytes.Buffer
var enc = msgpack.NewEncoder(&buf)

// integers in their shortest form, as the Iris and Rust versions write them
func init() { enc.UseCompactInts(true) }

func encode(o *Order) []byte {
	buf.Reset()
	if err := enc.Encode(o); err != nil {
		panic(err)
	}
	return append([]byte(nil), buf.Bytes()...)
}

func decode(b []byte) Order {
	var o Order
	if err := msgpack.Unmarshal(b, &o); err != nil {
		panic(err)
	}
	return o
}

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

// 100,000 orders decoded and encoded again, 20 times.
func main() {
	seed := int64(1)
	var stream []byte
	var ends []int
	for i := int64(0); i < 100000; i++ {
		o := Order{ID: i * 7919, Paid: i%3 == 0, Items: []Item{}, Tags: []string{}, Codes: []int32{}}
		o.Customer = fmt.Sprintf("customer %d", next(&seed)%10000)
		for j, n := int64(0), 1+next(&seed)%4; j < n; j++ {
			name := fmt.Sprintf("item %d", next(&seed)%500)
			qty := int32(1 + next(&seed)%9)
			price := float64(next(&seed)%10000) / 100.0
			o.Items = append(o.Items, Item{name, qty, price})
		}
		for j, n := int64(0), next(&seed)%3; j < n; j++ {
			o.Tags = append(o.Tags, fmt.Sprintf("tag%d", j))
		}
		for j, n := int64(0), next(&seed)%6; j < n; j++ {
			o.Codes = append(o.Codes, int32(next(&seed)%100000))
		}
		stream = append(stream, encode(&o)...)
		ends = append(ends, len(stream))
	}
	total := int64(0)
	for round := 0; round < 20; round++ {
		start := 0
		for _, end := range ends {
			o := decode(stream[start:end])
			total += o.ID + int64(len(o.Customer)) + int64(len(o.Tags))
			for _, it := range o.Items {
				total += int64(it.Qty) + int64(len(it.Name)) + int64(it.Price)
			}
			for _, c := range o.Codes {
				total += int64(c)
			}
			total += int64(len(encode(&o)))
			start = end
		}
	}
	fmt.Println(total)
}
