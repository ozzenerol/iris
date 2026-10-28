package main

import (
	"database/sql"
	"fmt"

	_ "github.com/mattn/go-sqlite3"
)

const N = 100000
const K = 20000

type Person struct {
	ID    int64
	Name  string
	Email string
	Age   int64
	Score float64
}

// 100000 rows added as structs in one transaction, all of them read back,
// then 20000 found by their key and 20000 written back, in memory.
func main() {
	db, err := sql.Open("sqlite3", ":memory:")
	if err != nil {
		panic(err)
	}
	defer db.Close()
	db.SetMaxOpenConns(1)
	if _, err := db.Exec("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT NOT NULL, age INTEGER, score REAL)"); err != nil {
		panic(err)
	}

	rows := make([]Person, N)
	for i := int64(0); i < N; i++ {
		rows[i] = Person{Name: fmt.Sprintf("user %d", i), Email: fmt.Sprintf("user%d@example.com", i), Age: i % 90, Score: float64(i%1000) / 4.0}
	}
	tx, _ := db.Begin()
	ins, err := tx.Prepare("INSERT INTO people (name, email, age, score) VALUES (?, ?, ?, ?)")
	if err != nil {
		panic(err)
	}
	var added int64
	for _, p := range rows {
		r, err := ins.Exec(p.Name, p.Email, p.Age, p.Score)
		if err != nil {
			panic(err)
		}
		n, _ := r.RowsAffected()
		added += n
	}
	ins.Close()
	tx.Commit()
	rows = nil

	var total int64
	q, err := db.Query("SELECT * FROM people")
	if err != nil {
		panic(err)
	}
	var all []Person
	for q.Next() {
		var p Person
		if err := q.Scan(&p.ID, &p.Name, &p.Email, &p.Age, &p.Score); err != nil {
			panic(err)
		}
		all = append(all, p)
	}
	q.Close()
	for _, p := range all {
		total += p.ID + int64(len(p.Name)) + int64(len(p.Email)) + p.Age + int64(p.Score)
	}
	all = nil

	tx, _ = db.Begin()
	find, _ := tx.Prepare("SELECT * FROM people WHERE id = ?")
	upd, _ := tx.Prepare("UPDATE people SET name = ?, email = ?, age = ?, score = ? WHERE id = ?")
	for k := int64(0); k < K; k++ {
		var p Person
		if err := find.QueryRow(k*5 + 1).Scan(&p.ID, &p.Name, &p.Email, &p.Age, &p.Score); err != nil {
			panic(err)
		}
		total += p.Age
		p.Age++
		r, err := upd.Exec(p.Name, p.Email, p.Age, p.Score, p.ID)
		if err != nil {
			panic(err)
		}
		n, _ := r.RowsAffected()
		total += n
	}
	find.Close()
	upd.Close()
	tx.Commit()
	fmt.Printf("%d %d\n", added, total)
}
