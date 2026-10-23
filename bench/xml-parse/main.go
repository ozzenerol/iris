package main

import (
	"bytes"
	"encoding/xml"
	"fmt"
	"io"
	"strings"
)

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

// A 30 MB document of items made in memory, read as events: elements,
// attributes and bytes of text.
func main() {
	var doc strings.Builder
	doc.WriteString("<catalog>")
	seed := int64(42)
	for i := 0; doc.Len() < 30000000; i++ {
		r := next(&seed)
		fmt.Fprintf(&doc, "<item id=\"%d\" kind=\"k%d\"><name>Item %d &amp; co</name><price>%d</price><tags><tag>a%d</tag><tag>b&lt;%d&gt;</tag></tags></item>", i, r%7, i, r%10000, r%13, r%5)
	}
	doc.WriteString("</catalog>")
	d := xml.NewDecoder(bytes.NewReader([]byte(doc.String())))
	var elements, attrs, text int
	for {
		tok, err := d.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			panic(err)
		}
		switch t := tok.(type) {
		case xml.StartElement:
			elements++
			attrs += len(t.Attr)
		case xml.CharData:
			text += len(t)
		}
	}
	fmt.Println(elements, attrs, text)
}
