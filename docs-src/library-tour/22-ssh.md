# SSH

SSH runs commands on other machines, copies files to them and reaches ports behind them. The `ssh` module is a client and a server. The client logs in to OpenSSH servers and others and runs commands. The server takes logins from `ssh`, `sftp` and other clients, and runs shells and commands for them.

## Running a command

`ssh.dial(host, port, config)` connects, checks the server's key, logs in and gives a `Client`. `run(command)` runs a command and waits for it to end. It gives the command's output, its errors and its exit status in a `process.Output` (see [Processes](06-processes.html)):

```iris
import ssh

!void main() {
    scoped key := try ssh.loadKey("/home/ada/.ssh/id_ed25519")
    c := try ssh.dial("db1.example.com", 22, ssh.Config{user: "ada", key: &key})
    defer c.close()

    scoped out := try c.run("df -h /var")
    print(out.stdout)
    if !out.ok() {
        print("df failed with {out.status}: {out.stderr}")
    }
}
```

A status other than 0 is no error: the command ran and ended that way. The errors are `SshError` codes:

- `connect`: the server can't be reached.
- `hostKey`: its key isn't the one known for it.
- `auth`: the login is refused.
- `closed`: the connection went away.

`run(command, input)` also gives the command something to read on its standard input.

## Logging in

`Config.key` is an Ed25519 key, the kind `ssh-keygen -t ed25519` makes. `loadKey` reads its private key file. A key with a passphrase has to have it taken off first (`ssh-keygen -p -N ''`). `Config.password` logs in with a password instead. With both, the key is tried first.

`ssh.generateKey(comment)` makes a new key. `privateText()` gives it in the format of `~/.ssh/id_ed25519`, and `publicText()` gives the line that goes in a server's `~/.ssh/authorized_keys`:

```iris run
import ssh, str

void main() {
    scoped key := ssh.generateKey("deploy@ci")
    scoped String private = key.privateText()
    scoped String public = key.publicText()
    scoped String fingerprint = key.fingerprint()
    scoped List[str] lines = str.lines(private)
    scoped List[str] fields = str.fields(public)
    print(lines.at(0))
    print("{fields.at(0)} ... {fields.at(2)}")
    print("{fingerprint[0:7]} and {fingerprint.len - 7} more characters")
}
```

```output
-----BEGIN OPENSSH PRIVATE KEY-----
ssh-ed25519 ... deploy@ci
SHA256: and 43 more characters
```

`fingerprint()` is the key's hash as `ssh-keygen -l` shows it.

## The server's key

Before it logs in, `dial` checks that the server is the one it should be. As `ssh` does, it looks the server's key up in `~/.ssh/known_hosts`, with hashed names too, and with `[host]:port` for a port other than 22. `Config.knownHosts` names another file.

A server that isn't in the file is refused with `SshError.hostKey`. The message has the server's fingerprint and the line to add once the fingerprint is checked. A server whose key has changed is refused too. `Config.hostKey` gives the key directly, as a public key line or a `SHA256:` fingerprint. `Client.serverKey` is the key the server showed. `Config.anyHostKey` trusts any key and is only for tests.

## Output as it comes

`exec(command)` starts a command and gives a `Channel` without waiting for the command to end. `read` gives its output as it comes, `readStderr` its errors, and `write` gives it input. `closeWrite` ends the input, and `wait` gives the exit status once the command ends:

```iris
ch := try c.exec("journalctl -f -u nginx")
defer ch.close()
u8[] buf = alloc(u8, 32KiB)
defer free(buf)
for {
    n := try ch.read(buf)
    if n == 0 {
        break
    }
    io.write(buf[0:n])
}
print("ended with {ch.wait()}")
```

`shell()` starts the user's shell on a terminal, as `ssh` does when it logs in. `resize(rows, cols)` gives the terminal a new size, and `signal("INT")` sends the command a signal. `subsystem("sftp")` starts the server's sftp subsystem.

A connection carries many channels at once, each with its own flow, so a slow reader of one doesn't hold up the others. Several tasks can each run their own command on one `Client`. A `Client` and its channels stay on the thread that made them.

## Reaching a port behind the server

`forward(host, port)` opens a connection from the server to `host:port`, as `ssh -L` does. What is written to the `Channel` goes there, and what comes back can be read. `pipe(&conn)` copies between the channel and a `net.Conn` both ways until both ends are done. Here a local port reaches a database that only the server can see:

```iris
import net, ssh

void tunnel(ssh.Client *c, net.Conn local) {
    defer local.close()
    ch := c.forward("10.0.0.5", 5432) catch e {
        return
    }
    defer ch.close()
    ch.pipe(&local)
}

!void main() {
    scoped key := try ssh.loadKey("/home/ada/.ssh/id_ed25519")
    c := try ssh.dial("bastion.example.com", 22, ssh.Config{user: "ada", key: &key})
    defer c.close()
    ln := try net.listen(15432, "127.0.0.1")
    for {
        local := try ln.accept()
        spawn tunnel(&c, local)
    }
}
```

## A server

An `ssh.Server` holds the server's own key and the functions that decide things. `password` and `publicKey` each take a user and a password or a key and say whether the login is allowed. A way of logging in whose function is `null` isn't offered. `handle` runs each shell, command or subsystem a client asks for. `ssh.serve(port, server)` listens and serves each connection in a task.

The handler gets a `Session`, with the user, the `kind` (`shell`, `exec` or `subsystem`) and the `command`. It reads and writes the session and sets `status`, the exit status the client gets when the handler returns. This program runs a server and a client together:

```iris run
import net, ssh

ssh.Key userKey

bool checkKey(str user, str key) {
    scoped String want = userKey.fingerprint()
    scoped String got = ssh.fingerprint(key) catch "".copy()
    return user == "ada" && got == want
}

!void handle(ssh.Session *s) {
    if s.command == "uptime" {
        try s.write("up 41 days\n")
        return
    }
    try s.writeStderr("{s.command}: not found\n")
    s.status = 127
}

void serveOn(net.Listener ln, ssh.Server s) {
    ssh.serveOn(ln, s) catch e { }
}

!void main() {
    userKey = ssh.generateKey()
    scoped hostKey := ssh.generateKey()
    ln := try net.listen(0, "127.0.0.1")
    spawn serveOn(ln, ssh.Server{hostKey: hostKey, publicKey: checkKey, handle: handle})

    scoped String known = hostKey.publicText()
    c := try ssh.dial("127.0.0.1", ln.port, ssh.Config{user: "ada", key: &userKey, hostKey: known})
    defer c.close()
    scoped up := try c.run("uptime")
    print("{up.stdout.trim()} ({up.status})")
    scoped bad := try c.run("reboot")
    print("{bad.stderr.trim()} ({bad.status})")
}
```

```output
up 41 days (0)
reboot: not found (127)
```

The server's key is kept in a file, so clients find the same key each time. Make it once with `generateKey` and write `privateText()` to a file only the server can read. `loadKey` reads it back at the start.

## Running programs for a session

`Session.run(argv)` runs a program for the session, as `sshd` does. When the client asked for a terminal, as `ssh` does for a shell, the program runs on a pseudo-terminal of the client's size, and a resize reaches it. Otherwise its standard input, output and errors are joined to the session's. Its exit status is the session's. When the client goes away, the program and what it started get a hangup.

`process.Options` sets the rest (see [Processes](06-processes.html)): `user` runs it as the user who logged in, which needs root, and `dir` starts it in their home. `ssh.authorizedKey(path, key)` looks a key up in an `authorized_keys` file, and `shadow.checkLogin` checks a password against the system's (see [Random numbers and cryptography](19-random-and-crypto.html)):

```iris
import os, process, shadow, ssh

bool checkPassword(str user, str password) => shadow.checkLogin(user, password) catch false

bool checkKey(str user, str key) {
    scoped u := os.lookupUser(user) catch e {
        return false
    }
    return ssh.authorizedKey("{u.home}/.ssh/authorized_keys", key)
}

!void handle(ssh.Session *s) {
    scoped u := try os.lookupUser(s.user)
    o := process.Options{user: s.user, dir: u.home}
    if s.kind == .subsystem {
        if s.command == "sftp" {
            _ = try s.run(["/usr/lib/openssh/sftp-server"], o)
        }
        return
    }
    if s.kind == .shell {
        _ = try s.run([u.shell, "-l"], o)
        return
    }
    _ = try s.run([u.shell, "-c", s.command], o)
}

!void main() {
    hostKey := try ssh.loadKey("/etc/iris-ssh/host_key")
    try ssh.serve(2222, ssh.Server{hostKey: hostKey, password: checkPassword, publicKey: checkKey, handle: handle})
}
```

`ssh -p 2222 ada@host`, `scp -P 2222` and `sftp -P 2222` all work with it. `Server.forward` takes a user, a host and a port and says whether that user may reach it through the server with `ssh -L`. While it is `null`, forwarding is refused.

## What it speaks

Connections use the curve25519-sha256 key exchange, Ed25519 keys for servers and users, and AES-256-GCM, AES-128-GCM or ChaCha20-Poly1305. Both sides make new keys after `Config.rekeyAfter` bytes, 1 GiB unless set, and whenever the other side asks. The key exchange is the strict one, which OpenSSH added against the Terrapin attack. Servers that have only RSA or ECDSA host keys, and keys with a passphrase, are not supported yet.
