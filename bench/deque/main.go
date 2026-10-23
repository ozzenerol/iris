package main

import "fmt"

const N = 10000000

// Go has no deque: a ring buffer that doubles when full.
type deque struct {
	buf        []int64
	head, size int
}

func (d *deque) grow() {
	if d.size < len(d.buf) {
		return
	}
	n := len(d.buf) * 2
	if n == 0 {
		n = 8
	}
	b := make([]int64, n)
	for i := 0; i < d.size; i++ {
		b[i] = d.buf[(d.head+i)&(len(d.buf)-1)]
	}
	d.buf, d.head = b, 0
}

func (d *deque) pushBack(v int64) {
	d.grow()
	d.buf[(d.head+d.size)&(len(d.buf)-1)] = v
	d.size++
}

func (d *deque) pushFront(v int64) {
	d.grow()
	d.head = (d.head - 1) & (len(d.buf) - 1)
	d.buf[d.head] = v
	d.size++
}

func (d *deque) popFront() int64 {
	v := d.buf[d.head]
	d.head = (d.head + 1) & (len(d.buf) - 1)
	d.size--
	return v
}

func (d *deque) popBack() int64 {
	d.size--
	return d.buf[(d.head+d.size)&(len(d.buf)-1)]
}

func (d *deque) at(i int) int64 { return d.buf[(d.head+i)&(len(d.buf)-1)] }

func main() {
	var d deque
	var sum int64
	most := 0
	for i := int64(0); i < N; i++ {
		d.pushBack(i)
		d.pushFront(i)
		if i%3 == 2 {
			sum += d.popFront()
			sum += d.popBack()
		}
		if d.size > most {
			most = d.size
		}
	}
	for i := 0; i < d.size; i++ {
		sum += d.at(i) % 7
	}
	for d.size > 0 {
		sum += d.popFront()
		if d.size > 0 {
			sum -= d.popBack()
		}
	}
	fmt.Println(most, sum)
}
