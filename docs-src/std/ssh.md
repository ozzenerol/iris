## A client and a server

`ssh.dial` logs in to a server and gives a `Client`, whose `run` runs a command and waits for its output and exit status. `exec`, `shell` and `forward` open channels that are read and written as the command or connection goes on. `ssh.serve` answers clients with the functions of an `ssh.Server`: one each to check passwords and keys, and one that handles each session, often with `Session.run`. The [SSH chapter](../library-tour/22-ssh.html) of the Library Tour has more. This program runs both sides:

```iris run
import net, ssh

!void handle(ssh.Session *s) => try s.write("hello {s.user}, you ran {s.command}\n")

bool anyPassword(str user, str password) => password == "secret"

void serveOn(net.Listener ln, ssh.Server s) {
    ssh.serveOn(ln, s) catch e { }
}

!void main() {
    scoped hostKey := ssh.generateKey()
    ln := try net.listen(0, "127.0.0.1")
    spawn serveOn(ln, ssh.Server{hostKey: hostKey, password: anyPassword, handle: handle})

    scoped String known = hostKey.publicText()
    c := try ssh.dial("127.0.0.1", ln.port, ssh.Config{user: "ada", password: "secret", hostKey: known})
    defer c.close()
    scoped out := try c.run("whoami")
    print(out.stdout.trim())
}
```

```output
hello ada, you ran whoami
```

The server's key is checked against `~/.ssh/known_hosts` unless `Config.hostKey` gives it. A server's key can be Ed25519, ECDSA or RSA. The keys this module makes and logs in with are Ed25519, in the formats `ssh-keygen` reads and writes, with or without a passphrase.
