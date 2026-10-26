package main

// Encodes 500000 orders to BSON and decodes each one back, and prints sums
// over what came back.

import (
	"fmt"
	"strconv"

	"go.mongodb.org/mongo-driver/v2/bson"
)

type Order struct {
	ID       bson.ObjectID `bson:"_id"`
	Customer string        `bson:"customer"`
	Email    string        `bson:"email"`
	Total    float64       `bson:"total"`
	Items    int64         `bson:"items"`
	Paid     bool          `bson:"paid"`
	Tags     []string      `bson:"tags"`
	Created  bson.DateTime `bson:"created"`
}

func main() {
	o := Order{Customer: "Ada Lovelace", Email: "ada@example.com", Paid: true, Tags: []string{"express", "gift", "fragile"}}
	for i := 0; i < 12; i++ {
		o.ID[i] = byte(i * 17)
	}
	total := 0.0
	sum := int64(0)
	for i := int64(0); i < 500000; i++ {
		o.Total = float64(i) * 0.5
		o.Items = i % 17
		o.Created = bson.DateTime(1792000000000 + i)
		buf, err := bson.Marshal(&o)
		if err != nil {
			panic(err)
		}
		var back Order
		if err := bson.Unmarshal(buf, &back); err != nil {
			panic(err)
		}
		total += back.Total
		sum += back.Items + int64(len(back.Customer)) + int64(len(back.Tags)) + int64(back.ID[11]) + int64(back.Created)%7 + int64(len(buf))
	}
	fmt.Println(strconv.FormatFloat(total, 'f', -1, 64), sum)
}
