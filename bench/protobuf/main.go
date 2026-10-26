// The same messages read and written by hand with protowire, as there is
// no protoc here to generate the code.
package main

import (
	"fmt"
	"math"

	"google.golang.org/protobuf/encoding/protowire"
)

type Item struct {
	Name  string
	Qty   int32
	Price float64
}

type Order struct {
	ID       int64
	Customer string
	Items    []Item
	Tags     []string
	Codes    []int32
	Paid     bool
}

func appendItem(b []byte, it *Item) []byte {
	if it.Name != "" {
		b = protowire.AppendTag(b, 1, protowire.BytesType)
		b = protowire.AppendString(b, it.Name)
	}
	if it.Qty != 0 {
		b = protowire.AppendTag(b, 2, protowire.VarintType)
		b = protowire.AppendVarint(b, uint64(int64(it.Qty)))
	}
	if it.Price != 0 {
		b = protowire.AppendTag(b, 3, protowire.Fixed64Type)
		b = protowire.AppendFixed64(b, math.Float64bits(it.Price))
	}
	return b
}

func appendOrder(b []byte, o *Order) []byte {
	if o.ID != 0 {
		b = protowire.AppendTag(b, 1, protowire.VarintType)
		b = protowire.AppendVarint(b, uint64(o.ID))
	}
	if o.Customer != "" {
		b = protowire.AppendTag(b, 2, protowire.BytesType)
		b = protowire.AppendString(b, o.Customer)
	}
	for i := range o.Items {
		b = protowire.AppendTag(b, 3, protowire.BytesType)
		at := len(b)
		b = append(b, 0)
		b = appendItem(b, &o.Items[i])
		n := len(b) - at - 1
		if n < 128 {
			b[at] = byte(n)
		} else {
			body := append([]byte(nil), b[at+1:]...)
			b = protowire.AppendVarint(b[:at], uint64(n))
			b = append(b, body...)
		}
	}
	for _, t := range o.Tags {
		b = protowire.AppendTag(b, 4, protowire.BytesType)
		b = protowire.AppendString(b, t)
	}
	if len(o.Codes) > 0 {
		n := 0
		for _, c := range o.Codes {
			n += protowire.SizeVarint(uint64(int64(c)))
		}
		b = protowire.AppendTag(b, 5, protowire.BytesType)
		b = protowire.AppendVarint(b, uint64(n))
		for _, c := range o.Codes {
			b = protowire.AppendVarint(b, uint64(int64(c)))
		}
	}
	if o.Paid {
		b = protowire.AppendTag(b, 6, protowire.VarintType)
		b = protowire.AppendVarint(b, 1)
	}
	return b
}

func readItem(b []byte) (Item, error) {
	var it Item
	for len(b) > 0 {
		num, typ, n := protowire.ConsumeTag(b)
		if n < 0 {
			return it, protowire.ParseError(n)
		}
		b = b[n:]
		switch {
		case num == 1 && typ == protowire.BytesType:
			v, n := protowire.ConsumeString(b)
			if n < 0 {
				return it, protowire.ParseError(n)
			}
			it.Name, b = v, b[n:]
		case num == 2 && typ == protowire.VarintType:
			v, n := protowire.ConsumeVarint(b)
			if n < 0 {
				return it, protowire.ParseError(n)
			}
			it.Qty, b = int32(v), b[n:]
		case num == 3 && typ == protowire.Fixed64Type:
			v, n := protowire.ConsumeFixed64(b)
			if n < 0 {
				return it, protowire.ParseError(n)
			}
			it.Price, b = math.Float64frombits(v), b[n:]
		default:
			n := protowire.ConsumeFieldValue(num, typ, b)
			if n < 0 {
				return it, protowire.ParseError(n)
			}
			b = b[n:]
		}
	}
	return it, nil
}

func readOrder(b []byte) (Order, error) {
	var o Order
	for len(b) > 0 {
		num, typ, n := protowire.ConsumeTag(b)
		if n < 0 {
			return o, protowire.ParseError(n)
		}
		b = b[n:]
		switch {
		case num == 1 && typ == protowire.VarintType:
			v, n := protowire.ConsumeVarint(b)
			if n < 0 {
				return o, protowire.ParseError(n)
			}
			o.ID, b = int64(v), b[n:]
		case num == 2 && typ == protowire.BytesType:
			v, n := protowire.ConsumeString(b)
			if n < 0 {
				return o, protowire.ParseError(n)
			}
			o.Customer, b = v, b[n:]
		case num == 3 && typ == protowire.BytesType:
			v, n := protowire.ConsumeBytes(b)
			if n < 0 {
				return o, protowire.ParseError(n)
			}
			it, err := readItem(v)
			if err != nil {
				return o, err
			}
			o.Items, b = append(o.Items, it), b[n:]
		case num == 4 && typ == protowire.BytesType:
			v, n := protowire.ConsumeString(b)
			if n < 0 {
				return o, protowire.ParseError(n)
			}
			o.Tags, b = append(o.Tags, v), b[n:]
		case num == 5 && typ == protowire.BytesType:
			v, n := protowire.ConsumeBytes(b)
			if n < 0 {
				return o, protowire.ParseError(n)
			}
			b = b[n:]
			for len(v) > 0 {
				c, k := protowire.ConsumeVarint(v)
				if k < 0 {
					return o, protowire.ParseError(k)
				}
				o.Codes, v = append(o.Codes, int32(c)), v[k:]
			}
		case num == 6 && typ == protowire.VarintType:
			v, n := protowire.ConsumeVarint(b)
			if n < 0 {
				return o, protowire.ParseError(n)
			}
			o.Paid, b = v != 0, b[n:]
		default:
			n := protowire.ConsumeFieldValue(num, typ, b)
			if n < 0 {
				return o, protowire.ParseError(n)
			}
			b = b[n:]
		}
	}
	return o, nil
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
		o := Order{ID: i * 7919, Paid: i%3 == 0}
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
		stream = appendOrder(stream, &o)
		ends = append(ends, len(stream))
	}
	total := int64(0)
	for round := 0; round < 20; round++ {
		start := 0
		for _, end := range ends {
			o, err := readOrder(stream[start:end])
			if err != nil {
				panic(err)
			}
			total += o.ID + int64(len(o.Customer)) + int64(len(o.Tags))
			for _, it := range o.Items {
				total += int64(it.Qty) + int64(len(it.Name)) + int64(it.Price)
			}
			for _, c := range o.Codes {
				total += int64(c)
			}
			b := appendOrder(nil, &o)
			total += int64(len(b))
			start = end
		}
	}
	fmt.Println(total)
}
