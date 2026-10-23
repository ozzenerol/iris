package main

import "net/http"

func main() {
	http.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		w.Write([]byte("hello, world\n"))
	})
	http.ListenAndServe("127.0.0.1:9000", nil)
}
