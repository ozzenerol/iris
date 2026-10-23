package main

import (
	"bytes"
	"fmt"
	"html/template"
)

type Row struct {
	ID   int
	Name string
	Qty  int
}

type Page struct {
	Title string
	Rows  []Row
}

// A page with a table of 1000 rows whose text needs escaping, rendered
// 2000 times.
func main() {
	t := template.Must(template.New("page").Parse(`<html><head><title>{{.Title}}</title></head><body><table>{{range .Rows}}<tr><td>{{.ID}}</td><td>{{.Name}}</td><td>{{.Qty}}</td></tr>{{end}}</table></body></html>`))
	p := Page{Title: "Orders & <returns>"}
	for i := 0; i < 1000; i++ {
		p.Rows = append(p.Rows, Row{ID: i, Name: fmt.Sprintf("item <%d> & more", i), Qty: i % 17})
	}
	total := 0
	var buf bytes.Buffer
	for k := 0; k < 2000; k++ {
		buf.Reset()
		if err := t.Execute(&buf, p); err != nil {
			panic(err)
		}
		total += buf.Len()
	}
	fmt.Println(total)
}
