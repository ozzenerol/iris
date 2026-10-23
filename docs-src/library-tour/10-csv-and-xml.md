# CSV and XML

Spreadsheets, exports and reports often come as CSV, and many older services and feeds speak XML. The `csv` module reads and writes comma-separated values, and the `xml` module reads a document into a tree or a step at a time, and writes one back.

## Reading CSV

`csv.parse(text)` reads every record into a `List` of `csv.Record`. A record's `len` is its number of fields, and `at(i)` gives field `i`, or `""` past the end. A field in double quotes may hold commas, line breaks, and quotes written twice:

```iris run
import csv

!void main() {
    text := "host,port,note\ndb,5432,\"primary, in rack 3\"\ncache,6379,\"says \"\"hi\"\"\"\n"
    rows := try csv.parse(text)
    defer rows.freeEach()
    for r in rows.items()[1:] {
        print("{r.at(0)}:{r.at(1)} [{r.at(2)}]")
    }
    print("{rows.len} records, {rows.at(0).len} fields each")
}
```

```output
db:5432 [primary, in rack 3]
cache:6379 [says "hi"]
3 records, 3 fields each
```

The first record is the header here, so the loop starts after it. `freeEach` frees every record and then the list.

A large file is read a record at a time with `csv.reader(text)`, whose `next` gives the next record and fails with `CsvError.end` when there are no more. `sep` sets another separator, and `line` is the line the next record starts on, for messages about bad input:

```iris run
import csv

!void main() {
    text := "name;bytes\nindex.html;5120\nlogo.png;20480\napp.js;x\n"
    rd := csv.reader(text)
    rd.sep = ";"
    scoped csv.Record header = try rd.next()
    i64 total = 0
    for {
        at := rd.line
        r := rd.next() catch e {
            if e.code != csv.CsvError.end {
                print("{e.message}")
            }
            break
        }
        defer r.free()
        n := r.at(1).parseInt() catch e {
            print("line {at}: {r.at(1)} isn't a number of bytes")
            continue
        }
        total += n
    }
    print("{total} bytes in all")
}
```

```output
line 4: x isn't a number of bytes
25600 bytes in all
```

A quoted field without its closing quote fails with `CsvError.quote` and the line it started on.

## Writing CSV

`csv.row(fields)` gives one line of CSV, ending in `\n`. A field is put in quotes only when it needs them: when it holds the separator, a quote or a line break. `csv.quote(field)` does that to one field:

```iris run
import csv

void main() {
    String out
    defer out.free()
    scoped String header = csv.row(["host", "status", "note"])
    out.append(header)
    str[3] a = ["api", "up", "fine"]
    str[3] b = ["db", "down", "disk full, \"/var\" at 100%"]
    for r in [a[:], b[:]] {
        scoped String line = csv.row(r)
        out.append(line)
    }
    print(out)
}
```

```output
host,status,note
api,up,fine
db,down,"disk full, ""/var"" at 100%"

```

The second argument of `csv.row` and `csv.quote` is the separator, `","` when it is left out.

## Reading XML into a tree

`xml.parse(text)` reads a document into its root `xml.Element`. An element has a `name`, its `attrs`, its `children` and its `text`, the text right inside it. `child(name)` gives the first child with that name, or `null`, and `attr(name)` gives an attribute's value, or `""`. Entities such as `&amp;` are decoded, and text that is only spaces between tags is left out:

```iris run
import xml

!void main() {
    text := r"""
        <?xml version="1.0"?>
        <feed>
          <title>Status &amp; incidents</title>
          <entry id="41" severity="minor">
            <title>Slow logins</title>
            <updated>2026-10-23T08:10:00Z</updated>
          </entry>
          <entry id="42" severity="major">
            <title>API down in eu-west</title>
            <updated>2026-10-24T09:00:00Z</updated>
          </entry>
        </feed>
        """
    scoped xml.Element feed = try xml.parse(text)
    print(feed.child("title").text)
    for e in feed.children {
        if e.name != "entry" {
            continue
        }
        print("#{e.attr("id")} {e.attr("severity")}: {e.child("title").text} at {e.child("updated").text}")
    }
}
```

```output
Status & incidents
#41 minor: Slow logins at 2026-10-23T08:10:00Z
#42 major: API down in eu-west at 2026-10-24T09:00:00Z
```

`child` gives a pointer, so a missing element is checked against `null` before it is used. Text mixed with elements, as in `<p>a <b>bold</b> word</p>`, is kept as children with no name: `isText()` tells them apart, and `allText()` joins all the text inside an element:

```iris run
import xml

!void main() {
    scoped xml.Element p = try xml.parse("<p>Restart <code>nginx</code> after <b>every</b> change.</p>")
    scoped String all = p.allText()
    print(all)
    for c in p.children {
        if c.isText() {
            print("text [{c.text}]")
        } else {
            print("<{c.name}> [{c.text}]")
        }
    }
    print("has a link: {p.child("a") != null}")
}
```

```output
Restart nginx after every change.
text [Restart ]
<code> [nginx]
text [ after ]
<b> [every]
text [ change.]
has a link: false
```

## Reading XML a step at a time

A large document doesn't need to be held as a tree. `xml.reader(text)` reads it a step at a time: each `next` gives an `xml.Event` that is a `.start` tag with its `name` and attributes, a `.text`, or an `.end` tag. At the end of the document, `next` fails with `XmlError.end`. This adds up the sizes in a listing without building the tree:

```iris run
import xml

!void main() {
    rd := xml.reader(r"""<ListBucketResult><Contents><Key>a.log</Key><Size>1200</Size></Contents><Contents><Key>b.log</Key><Size>800</Size></Contents></ListBucketResult>""")
    defer rd.free()
    i64 total = 0
    i64 files = 0
    bool inSize = false
    for {
        ev := rd.next() catch e {
            if e.code != xml.XmlError.end {
                print("{e.code}: {e.message}")
            }
            break
        }
        match ev.kind {
            .start => inSize = ev.name == "Size"
            .text => {
                if inSize {
                    total += ev.text.parseInt() catch 0
                    files += 1
                }
            }
            .end => inSize = false
        }
        ev.free()
    }
    print("{files} files, {total} bytes")
}
```

```output
2 files, 2000 bytes
```

Text that isn't well-formed XML fails with `XmlError.syntax` and the line: a tag closed by the wrong end tag, one never closed, an attribute without quotes, or a second root element:

```iris run
import xml

void main() {
    for text in ["<a><b></a>", "<a x=1/>", "<a/><b/>"] {
        _ = xml.parse(text) catch e {
            print("{e.code}: {e.message}")
            xml.element("")
        }
    }
}
```

```output
XmlError.syntax: line 1: </a> closes <b>
XmlError.syntax: line 1: an attribute's value needs quotes
XmlError.syntax: line 1: a second root element
```

## Writing XML

`xml.element(name)` makes an element. `set` gives it an attribute, `addText` text, and `add` a child, which the parent owns after. `write` gives the whole tree as XML on one line, with `&`, `<`, `>` and quotes written as entities:

```iris run
import xml

void main() {
    scoped xml.Element urls = xml.element("urlset")
    urls.set("xmlns", "http://www.sitemaps.org/schemas/sitemap/0.9")
    for page in ["/", "/docs?a=1&b=2"] {
        u := xml.element("url")
        loc := xml.element("loc")
        scoped String full = format("https://example.com{page}")
        loc.addText(full)
        u.add(loc)
        urls.add(u)
    }
    scoped String text = urls.write()
    print(text)
}
```

```output
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9"><url><loc>https://example.com/</loc></url><url><loc>https://example.com/docs?a=1&amp;b=2</loc></url></urlset>
```

`xml.escape(text)` gives any text with those characters written as entities, for XML written by hand.
