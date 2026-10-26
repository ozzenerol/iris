package main

// Against a MySQL server at $IRIS_BENCH_MYSQL: 1000 rows inserted in a
// transaction, 20000 rows read one at a time by id with a prepared
// statement, then the whole table read 100 times, all on one connection.

import (
	"database/sql"
	"fmt"
	"net/url"
	"os"
	"strings"

	_ "github.com/go-sql-driver/mysql"
)

type Item struct {
	ID    int64
	Name  string
	Email string
	Score float64
	Qty   int64
}

// mysql://user:pass@host:port/db as the driver's user:pass@tcp(host:port)/db
func dsn(address string) string {
	u, err := url.Parse(address)
	if err != nil {
		panic(err)
	}
	pass, _ := u.User.Password()
	return fmt.Sprintf("%s:%s@tcp(%s)/%s?multiStatements=true", u.User.Username(), pass, u.Host, strings.TrimPrefix(u.Path, "/"))
}

func main() {
	address := os.Getenv("IRIS_BENCH_MYSQL")
	if address == "" {
		address = "mysql://iris:secret@127.0.0.1:3306/iris_test?allowPublicKeyRetrieval=true"
	}
	db, err := sql.Open("mysql", dsn(address))
	if err != nil {
		panic(err)
	}
	db.SetMaxOpenConns(1)
	defer db.Close()
	if _, err := db.Exec("DROP TABLE IF EXISTS items; CREATE TABLE items (id int PRIMARY KEY, name varchar(50) NOT NULL, email varchar(100) NOT NULL, score double NOT NULL, qty int NOT NULL)"); err != nil {
		panic(err)
	}
	tx, err := db.Begin()
	if err != nil {
		panic(err)
	}
	ins, err := tx.Prepare("INSERT INTO items (id, name, email, score, qty) VALUES (?, ?, ?, ?, ?)")
	if err != nil {
		panic(err)
	}
	for i := 1; i < 1001; i++ {
		if _, err := ins.Exec(i, fmt.Sprintf("item %d", i), fmt.Sprintf("buyer%d@example.com", i), float64(i)*0.5, i%17); err != nil {
			panic(err)
		}
	}
	ins.Close()
	if err := tx.Commit(); err != nil {
		panic(err)
	}
	one, err := db.Prepare("SELECT id, name, email, score, qty FROM items WHERE id = ?")
	if err != nil {
		panic(err)
	}
	score := 0.0
	chars := int64(0)
	for i := 0; i < 20000; i++ {
		var it Item
		if err := one.QueryRow(i*7919%1000+1).Scan(&it.ID, &it.Name, &it.Email, &it.Score, &it.Qty); err != nil {
			panic(err)
		}
		score += it.Score
		chars += int64(len(it.Name)+len(it.Email)) + it.Qty
	}
	all, err := db.Prepare("SELECT id, name, email, score, qty FROM items WHERE qty >= ?")
	if err != nil {
		panic(err)
	}
	ids := int64(0)
	for i := 0; i < 100; i++ {
		rows, err := all.Query(0)
		if err != nil {
			panic(err)
		}
		var items []Item
		for rows.Next() {
			var it Item
			if err := rows.Scan(&it.ID, &it.Name, &it.Email, &it.Score, &it.Qty); err != nil {
				panic(err)
			}
			items = append(items, it)
		}
		rows.Close()
		for _, it := range items {
			ids += it.ID
		}
	}
	fmt.Println(int64(score), chars, ids)
}
