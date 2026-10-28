use rusqlite::{params, Connection};

const N: i64 = 100000;
const K: i64 = 20000;

struct Person {
    id: i64,
    name: String,
    email: String,
    age: i64,
    score: f64,
}

fn read(r: &rusqlite::Row) -> rusqlite::Result<Person> {
    Ok(Person { id: r.get(0)?, name: r.get(1)?, email: r.get(2)?, age: r.get(3)?, score: r.get(4)? })
}

// 100000 rows added as structs in one transaction, all of them read back,
// then 20000 found by their key and 20000 written back, in memory.
fn main() -> rusqlite::Result<()> {
    let mut db = Connection::open_in_memory()?;
    db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT NOT NULL, age INTEGER, score REAL)", [])?;

    let rows: Vec<Person> = (0..N)
        .map(|i| Person { id: 0, name: format!("user {}", i), email: format!("user{}@example.com", i), age: i % 90, score: (i % 1000) as f64 / 4.0 })
        .collect();
    let tx = db.transaction()?;
    let mut added = 0;
    {
        let mut ins = tx.prepare_cached("INSERT INTO people (name, email, age, score) VALUES (?1, ?2, ?3, ?4)")?;
        for p in &rows {
            added += ins.execute(params![p.name, p.email, p.age, p.score])?;
        }
    }
    tx.commit()?;
    drop(rows);

    let mut total = 0i64;
    {
        let mut q = db.prepare("SELECT * FROM people")?;
        let all: Vec<Person> = q.query_map([], read)?.collect::<rusqlite::Result<_>>()?;
        for p in &all {
            total += p.id + p.name.len() as i64 + p.email.len() as i64 + p.age + p.score as i64;
        }
    }

    let tx = db.transaction()?;
    {
        let mut find = tx.prepare_cached("SELECT * FROM people WHERE id = ?1")?;
        let mut upd = tx.prepare_cached("UPDATE people SET name = ?1, email = ?2, age = ?3, score = ?4 WHERE id = ?5")?;
        for k in 0..K {
            let mut p = find.query_row([k * 5 + 1], read)?;
            total += p.age;
            p.age += 1;
            total += upd.execute(params![p.name, p.email, p.age, p.score, p.id])? as i64;
        }
    }
    tx.commit()?;
    println!("{} {}", added, total);
    Ok(())
}
