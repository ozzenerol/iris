## Sending mail

`smtp.send` connects to the server, sends one mail and closes. For several mails, `connect` gives a `Client` that sends each one on the same connection:

```iris
import smtp

!void main() {
    mail := smtp.Mail{
        from: "Alerts <alerts@example.com>",
        to: ["ops@example.com"],
        subject: "disk 91% full on db1",
        text: "/var on db1 is at 91%.",
        html: "<p>/var on <b>db1</b> is at 91%.</p>",
    }
    try smtp.send("smtps://alerts:secret@mail.example.com", mail)
}
```

With both `text` and `html`, the mail carries both, and the mail program shows the one it can. A subject with characters beyond ASCII is encoded as mail headers need. `bcc` recipients get the mail without being named in it.

A server that refuses a recipient fails the send with `SmtpError.rejected` and its reply, and the `Client` can send the next mail. A line break in `from`, `subject` or an address is an error, so text from users can't add headers.
