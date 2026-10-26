package main

// Against a MongoDB server at $IRIS_BENCH_MONGO: 1000 documents inserted one
// at a time, 20000 documents found one at a time by n, then the whole
// collection read 100 times, all on one connection.

import (
	"context"
	"fmt"
	"os"

	"go.mongodb.org/mongo-driver/v2/bson"
	"go.mongodb.org/mongo-driver/v2/mongo"
	"go.mongodb.org/mongo-driver/v2/mongo/options"
)

type Item struct {
	N     int64   `bson:"n"`
	Name  string  `bson:"name"`
	Email string  `bson:"email"`
	Score float64 `bson:"score"`
	Qty   int64   `bson:"qty"`
}

func main() {
	address := os.Getenv("IRIS_BENCH_MONGO")
	if address == "" {
		address = "mongodb://iris:secret@127.0.0.1:27017/iris_test?authSource=admin"
	}
	ctx := context.Background()
	client, err := mongo.Connect(options.Client().ApplyURI(address).SetMaxPoolSize(1).SetDirect(true))
	if err != nil {
		panic(err)
	}
	defer client.Disconnect(ctx)
	items := client.Database("iris_test").Collection("items")
	if err := items.Drop(ctx); err != nil {
		panic(err)
	}
	if _, err := items.Indexes().CreateOne(ctx, mongo.IndexModel{Keys: bson.D{{Key: "n", Value: 1}}, Options: options.Index().SetUnique(true)}); err != nil {
		panic(err)
	}
	for i := int64(1); i <= 1000; i++ {
		it := Item{N: i, Name: fmt.Sprintf("item %d", i), Email: fmt.Sprintf("buyer%d@example.com", i), Score: float64(i) * 0.5, Qty: i % 17}
		if _, err := items.InsertOne(ctx, &it); err != nil {
			panic(err)
		}
	}
	score := 0.0
	chars := int64(0)
	for i := int64(0); i < 20000; i++ {
		var it Item
		if err := items.FindOne(ctx, bson.D{{Key: "n", Value: i*7919%1000 + 1}}).Decode(&it); err != nil {
			panic(err)
		}
		score += it.Score
		chars += int64(len(it.Name)+len(it.Email)) + it.Qty
	}
	ids := int64(0)
	for j := 0; j < 100; j++ {
		cur, err := items.Find(ctx, bson.D{{Key: "qty", Value: bson.D{{Key: "$gte", Value: 0}}}})
		if err != nil {
			panic(err)
		}
		var found []Item
		if err := cur.All(ctx, &found); err != nil {
			panic(err)
		}
		for _, it := range found {
			ids += it.N
		}
	}
	fmt.Println(int64(score), chars, ids)
}
