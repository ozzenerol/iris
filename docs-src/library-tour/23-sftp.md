# SFTP and scp

SFTP works on the files of an SSH server: it copies them, lists folders, renames and removes. It runs over an SSH connection, so it logs in and checks the server as [SSH](22-ssh.html) does. The `ssh` module has its client and its server, and copies with scp for servers that have no SFTP.

## Copying files

`Client.sftp()` starts SFTP on a connection and gives an `Sftp`. `upload(local, remote)` copies a file to the server and `download(remote, local)` copies one back:

```iris
import ssh

!void main() {
    scoped key := try ssh.loadKey("/home/ada/.ssh/id_ed25519")
    c := try ssh.dial("files.example.com", 22, ssh.Config{user: "ada", key: &key})
    defer c.close()
    s := try c.sftp()
    defer s.close()

    try s.upload("report.pdf", "reports/2026-10.pdf")
    try s.download("logs/app.log", "app.log")
}
```

Paths without a `/` in front start from the folder SFTP starts in, usually the user's home. `realPath(".")` says which it is. `upload` keeps the local file's permission bits. Both keep 64 reads or writes of 32 KiB going at once, so a far server isn't waited on for each piece.

`readAll(path)` gives a whole file as a `String`, and `writeAll(path, data)` replaces one, as the `fs` functions of the same names do (see [Files](02-files.html)).

## Folders and names

`list(path)` gives the names in a folder, each with an `fs.Info` as `fs.lstat` gives it: the size, whether it is a folder or a link, the permission bits, the owner and the times:

```iris
scoped entries := try s.list("reports")
for e in entries {
    kind := "/" if e.info.isDir else ""
    print("{e.name}{kind} {e.info.size} bytes, changed {e.info.modified}")
    e.name.free()
}
```

`stat` and `lstat` tell about one path and `exists` says whether something is there. `mkdir`, `mkdirAll`, `rmdir` and `remove` make and remove folders and files. `rename(from, to)` renames, replacing what is at `to` when the server allows it, as OpenSSH's does. `symlink(target, path)` and `readLink(path)` make and read symbolic links, and `chmod` and `setTimes` change permissions and times.

The errors are `SftpError` codes from what the server said: `notFound`, `permission`, `failure`, `unsupported`, `badMessage` and `eof`. A connection that goes away is `SshError.closed`.

## Open files

`open(path)` opens a file to read, `create(path)` to write from empty, and `openAppend(path)` to add to its end. A `RemoteFile` reads and writes from its `offset`, which moves on, or at an offset given to `readAt` and `writeAt`. `info()` tells about it, and `close` closes it:

```iris
f := try s.open("logs/app.log")
defer f.close()
info := try f.info()
u8[4096] tail
n := try f.readAt(tail[0:4096], info.size - 4096 if info.size > 4096 else 0)
io.write(tail[0:n])
```

## An SFTP server

`ssh.serveSftp(session, root)` serves SFTP for a session that asked for the `sftp` subsystem, which is what `sftp`, `scp` and file managers ask for. The client sees `root` as `/` and its paths stay inside it. Symbolic links the client makes point inside it too. The files are read and written as the user the server runs as. This program runs a server and a client together:

```iris run
import fs, net, ssh

String root
ssh.Key userKey

bool checkKey(str user, str key) {
    scoped String want = userKey.fingerprint()
    scoped String got = ssh.fingerprint(key) catch "".copy()
    return got == want
}

!void handle(ssh.Session *s) {
    if s.kind == .subsystem && s.command == "sftp" {
        try ssh.serveSftp(s, root)
        return
    }
    s.status = 1
}

void serveOn(net.Listener ln, ssh.Server s) {
    ssh.serveOn(ln, s) catch e { }
}

!void main() {
    root = try fs.tempDir("files")
    defer fs.removeAll(root) catch e { }
    userKey = ssh.generateKey()
    scoped hostKey := ssh.generateKey()
    ln := try net.listen(0, "127.0.0.1")
    spawn serveOn(ln, ssh.Server{hostKey: hostKey, publicKey: checkKey, handle: handle})

    scoped String known = hostKey.publicText()
    c := try ssh.dial("127.0.0.1", ln.port, ssh.Config{user: "ada", key: &userKey, hostKey: known})
    defer c.close()
    s := try c.sftp()
    defer s.close()
    try s.mkdir("notes")
    try s.writeAll("notes/todo.txt", "buy cables\n")
    try s.rename("notes/todo.txt", "notes/done.txt")
    scoped entries := try s.list("notes")
    for e in entries {
        print("{e.name}: {e.info.size} bytes")
        e.name.free()
    }
    scoped String text = try s.readAll("/notes/done.txt")
    print(text.trim())
}
```

```output
done.txt: 11 bytes
buy cables
```

To serve each user their own files, give each a root of their own, such as their home, and run the server as a user that may read them.

## scp

Some servers have no SFTP, only the `scp` program. `Client.scpUpload(local, remote)` and `Client.scpDownload(remote, local)` copy one file through it, keeping its permission bits:

```iris
try c.scpUpload("backup.tar.gz", "/var/backups/")
try c.scpDownload("/etc/nginx/nginx.conf", "nginx.conf")
```

`remote` may be a folder for `scpUpload`. When scp on the server refuses, the error is `SshError.refused` with its message.
