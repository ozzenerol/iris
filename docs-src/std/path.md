## Taking paths apart

The functions work on the text of a path and never look at the disk. `join` and `clean` give a new `String`; `base`, `dir`, `ext`, `stem` and `split` give a part of the path they are given:

```iris run
import path

!void main() {
    dir, name := path.split("/var/log/proxy/access.log")
    print("{dir} {name} {path.ext(name)}")
    scoped String c = path.clean("/var//log/./proxy/../app/")
    scoped String r = try path.rel("/var/log", "/var/www/index.html")
    print("{c} {r}")
}
```

```output
/var/log/proxy/ access.log .log
/var/log/app ../www/index.html
```
