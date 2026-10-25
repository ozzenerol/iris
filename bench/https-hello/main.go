package main

import "net/http"

// cert.pem and key.pem are in the folder it runs in
func main() {
	http.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		w.Write([]byte("hello, world\n"))
	})
	http.ListenAndServeTLS("127.0.0.1:9000", "cert.pem", "key.pem", nil)
}
