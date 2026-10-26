package main

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// The same protocol, read and written by hand with encoding/binary,
// checking each length as the Iris and Rust versions do.

type Msg struct {
	Version byte
	Kind    byte
	Flags   uint16
	Seq     uint32
	User    []byte
	Ports   []uint16
}

var errShort = errors.New("short")

func decode(b []byte) (Msg, error) {
	var m Msg
	if len(b) < 9 {
		return m, errShort
	}
	m.Version = b[0]
	m.Kind = b[1]
	if m.Kind < 1 || m.Kind > 3 {
		return m, errors.New("bad kind")
	}
	m.Flags = binary.BigEndian.Uint16(b[2:])
	m.Seq = binary.BigEndian.Uint32(b[4:])
	n := int(b[8])
	b = b[9:]
	if len(b) < n+1 {
		return m, errShort
	}
	m.User = append([]byte(nil), b[:n]...)
	count := int(b[n])
	b = b[n+1:]
	if len(b) < 2*count {
		return m, errShort
	}
	m.Ports = make([]uint16, count)
	for i := range m.Ports {
		m.Ports[i] = binary.BigEndian.Uint16(b[2*i:])
	}
	if len(b) != 2*count {
		return m, errors.New("extra")
	}
	return m, nil
}

func encode(m *Msg) ([]byte, error) {
	if len(m.User) > 255 || len(m.Ports) > 255 {
		return nil, errors.New("too long")
	}
	out := make([]byte, 0, 10+len(m.User)+2*len(m.Ports))
	out = append(out, m.Version, m.Kind)
	out = binary.BigEndian.AppendUint16(out, m.Flags)
	out = binary.BigEndian.AppendUint32(out, m.Seq)
	out = append(out, byte(len(m.User)))
	out = append(out, m.User...)
	out = append(out, byte(len(m.Ports)))
	for _, p := range m.Ports {
		out = binary.BigEndian.AppendUint16(out, p)
	}
	return out, nil
}

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

// 100,000 messages decoded and encoded again, 30 times.
func main() {
	seed := int64(1)
	var stream []byte
	var ends []int
	for i := 0; i < 100000; i++ {
		var user []byte
		n := int(3 + next(&seed)%10)
		for j := 0; j < n; j++ {
			user = append(user, "abcdefghij"[j%10])
		}
		ports := make([]uint16, next(&seed)%5)
		for j := range ports {
			ports[j] = uint16(next(&seed) % 65536)
		}
		m := Msg{Version: 1, Kind: 2, Flags: uint16(i % 7), Seq: uint32(i), User: user, Ports: ports}
		b, _ := encode(&m)
		stream = append(stream, b...)
		ends = append(ends, len(stream))
	}
	total := int64(0)
	for r := 0; r < 30; r++ {
		start := 0
		for _, end := range ends {
			m, err := decode(stream[start:end])
			if err != nil {
				panic(err)
			}
			total += int64(m.Seq) + int64(len(m.User)) + int64(m.Flags)
			for _, p := range m.Ports {
				total += int64(p)
			}
			b, _ := encode(&m)
			total += int64(len(b))
			start = end
		}
	}
	fmt.Println(total)
}
