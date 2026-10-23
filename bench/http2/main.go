package main

import "net/http"

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		w.Write([]byte("hello, world\n"))
	})
	var p http.Protocols
	p.SetHTTP1(true)
	p.SetUnencryptedHTTP2(true)
	s := &http.Server{Addr: "127.0.0.1:9000", Handler: mux, Protocols: &p}
	s.ListenAndServe()
}
