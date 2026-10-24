## Sending mail

`smtp.send` connects to the server, sends one mail and closes the connection. To send several mails, `connect` gives a `Client` that sends each one on the same connection:

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

A mail with both `text` and `html` carries both, and the mail program shows the one it can. A subject with characters beyond ASCII is encoded as mail headers need. `bcc` recipients get the mail without being named in it.

When the server refuses a recipient, the send fails with `SmtpError.rejected` and the server's reply, and the `Client` can still send the next mail. A line break in `from`, `subject` or an address is an error, so text from users can't add headers.

## Attachments

`attachments` sends files with the mail. Each `Attachment` has a `name`, its bytes in `data` and a `contentType`, which is `application/octet-stream` when empty. An attachment with an `id` is shown inside the HTML as `<img src="cid:id">` instead of as a file to save:

```iris
scoped String pdf = try fs.readAll("invoice.pdf")

try smtp.send(server, smtp.Mail{
    from: "billing@example.com",
    to: ["ada@example.com"],
    subject: "Invoice 1041",
    html: r"""<img src="cid:logo"><p>Your invoice is attached.</p>""",
    attachments: [
        smtp.Attachment{name: "logo.png", data: logo, contentType: "image/png", id: "logo"},
        smtp.Attachment{name: "invoice.pdf", data: pdf, contentType: "application/pdf"},
    ],
})
```
