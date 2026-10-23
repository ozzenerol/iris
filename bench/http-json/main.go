package main

import (
	"encoding/json"
	"net/http"
)

type Item struct {
	Name  string  `json:"name"`
	Qty   int64   `json:"qty"`
	Price float64 `json:"price"`
}

type Order struct {
	ID       int64    `json:"id"`
	Customer string   `json:"customer"`
	Items    []Item   `json:"items"`
	Tags     []string `json:"tags"`
}

type Total struct {
	ID    int64   `json:"id"`
	Count int     `json:"count"`
	Total float64 `json:"total"`
}

func main() {
	http.HandleFunc("POST /order", func(w http.ResponseWriter, r *http.Request) {
		var o Order
		if err := json.NewDecoder(r.Body).Decode(&o); err != nil {
			http.Error(w, err.Error(), 400)
			return
		}
		sum := 0.0
		for _, it := range o.Items {
			sum += float64(it.Qty) * it.Price
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(Total{o.ID, len(o.Items), sum})
	})
	http.ListenAndServe("127.0.0.1:9000", nil)
}
