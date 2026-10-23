# Sending email

Services send mail for sign-ups, password resets, alerts and reports. The `smtp` module sends it through a mail server over SMTP, the way a mail program does, with TLS and a login.

## A mail

A `smtp.Mail` holds what a mail has: `from`, `to`, `subject`, and the body as `text`, `html` or both. With both, the reader's mail program shows the one it can. `smtp.send(address, mail)` connects to the server, sends the mail and closes:

```iris
import smtp

!void main() {
    try smtp.send("smtps://alerts:secret@mail.example.com", smtp.Mail{
        from: "Alerts <alerts@example.com>",
        to: ["ops@example.com"],
        subject: "disk 91% full on db1",
        text: "/var on db1 is at 91%.",
        html: "<p>/var on <b>db1</b> is at 91%.</p>",
    })
}
```

`cc` names more recipients that everyone sees, `bcc` sends to recipients without naming them in the mail, and `replyTo` says where answers go. A subject with characters beyond ASCII is encoded as mail headers need it.

## The server's address

The server is given as an address, with the login in it:

| Address | Connection |
|---|---|
| `smtps://user:password@mail.example.com` | TLS from the start, on port 465 unless one is given |
| `smtp://user:password@mail.example.com:587` | plain at first, then TLS with STARTTLS |
| `smtp://localhost:25` | a local server, with no login |

On `smtp://`, the connection switches to TLS whenever the server offers it, and a password is never sent without TLS: when the server doesn't offer TLS, a login fails with `SmtpError.auth` before the password goes out. `?ca=file` trusts certificates other than the system's, for a server inside a company. A password with characters that mean something in a URL, such as `@` or `/`, is written with `url.encode` (see [Encodings and ids](40-encodings-and-ids.html)).

The password belongs in the environment, not in the code, so the address is read from a variable (see [Configuration files](38-configuration.html)):

```iris
import os, smtp

!void main() {
    server := try os.env("SMTP_URL")
    try smtp.send(server, smtp.Mail{from: "noreply@example.com", to: ["ada@example.com"], subject: "Welcome", text: "Thanks for signing up."})
}
```

## What goes over the wire

To see what `smtp` sends, this program runs a small mail server of its own in a task, which answers every command with success and prints what it gets. A real server would also check the sender and the recipients and keep the mail:

```iris run
import net, smtp

void serve(net.Listener ln) {
    c := ln.accept() catch e {
        return
    }
    defer c.close()
    c.write("220 test server\r\n") catch e { return }
    bool inData = false
    for {
        scoped String line = c.readLine() catch e {
            return
        }
        if inData {
            if line == "." {
                inData = false
                c.write("250 queued\r\n") catch e { return }
            } else if line.startsWith("Subject:") || line.startsWith("To:") || line.startsWith("Content-Type:") {
                shown, _, _ := line.cut(";")
                print("  {shown}")
            }
            continue
        }
        print(line)
        if line.startsWith("EHLO") {
            c.write("250 test server\r\n") catch e { return }
        } else if line == "DATA" {
            inData = true
            c.write("354 go on\r\n") catch e { return }
        } else if line == "QUIT" {
            c.write("221 bye\r\n") catch e { }
            return
        } else {
            c.write("250 ok\r\n") catch e { return }
        }
    }
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(ln)
    scoped String server = format("smtp://127.0.0.1:{ln.port}?helo=app.example.com")
    try smtp.send(server, smtp.Mail{
        from: "Shop <orders@example.com>",
        to: ["ada@example.com"],
        bcc: ["archive@example.com"],
        subject: "Order 1041 shipped",
        text: "Your order is on its way.",
        html: "<p>Your order is on its way.</p>",
    })
}
```

```output
EHLO app.example.com
MAIL FROM:<orders@example.com>
RCPT TO:<ada@example.com>
RCPT TO:<archive@example.com>
DATA
  To: ada@example.com
  Subject: Order 1041 shipped
  Content-Type: multipart/alternative
  Content-Type: text/plain
  Content-Type: text/html
QUIT
```

The `bcc` address got the mail with `RCPT TO`, and isn't in any header. The body has a plain text part and an HTML part, each encoded so that any text gets through. Every mail also gets a `Date` and a `Message-ID`.

## Several mails

`smtp.connect(address)` gives a `smtp.Client` that sends many mails on one connection, which is much faster than connecting for each. `close` ends it:

```iris
import smtp

!void main() {
    c := try smtp.connect("smtp://alerts:secret@mail.example.com:587")
    defer c.close()
    for to in ["ada@example.com", "linus@example.com", "grace@example.com"] {
        c.send(smtp.Mail{from: "alerts@example.com", to: [to], subject: "Maintenance tonight", text: "The API is down from 22:00 to 22:30 UTC."}) catch e {
            print("{to}: {e.message}")
        }
    }
}
```

One task uses a `Client` at a time. A program that sends from many tasks gives each its own, or keeps them in a pool (see [PostgreSQL and Redis](44-postgres-and-redis.html#a-pool-for-other-connections)).

## Errors

`SmtpError.connect` means the server can't be reached or TLS failed, and `auth` that the login was refused. `rejected` means the server refused the sender, a recipient or the mail, and the message has the server's reply, so a full mailbox or an unknown address shows why. After a `rejected` recipient, the `Client` can send the next mail.

A line break in `from`, `subject`, `replyTo` or an address fails with `SmtpError.rejected` before anything is sent, so text from a form can't add headers of its own, such as a `Bcc` to everyone.
