## A client and a broker

`mqtt.connect` logs in to a broker and gives a `Client`, which publishes with `publish`, subscribes with `subscribe` and takes messages with `next`. `mqtt.serve` is a broker, with the functions of an `mqtt.Broker` to check logins and topics. The [MQTT chapter](../library-tour/27-mqtt.html) of the Library Tour has more. This program runs both sides:

```iris run
import mqtt, net

void serveIt(net.Listener ln) {
    mqtt.serveOn(ln, mqtt.Broker{}) catch e { }
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    spawn serveIt(ln)
    c := try mqtt.connect("127.0.0.1", ln.port)
    defer c.close()
    _ = try c.subscribe("greetings/+")
    try c.publish("greetings/ada", "hello")
    m := try c.next()
    print("{m.topic}: {m.payload}")
    m.free()
}
```

```output
greetings/ada: hello
```
