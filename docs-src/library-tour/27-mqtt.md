# MQTT

MQTT carries small messages between devices and services through a broker. A client publishes a message to a topic, such as `home/kitchen/temp`, and the broker passes it to every client subscribed to that topic. The `mqtt` module is a client for MQTT 5 and 3.1.1 brokers such as Mosquitto, and a broker of its own.

## Publishing and subscribing

`mqtt.connect(host, port)` connects to a broker and logs in. `publish(topic, payload)` sends a message, `subscribe(filter)` asks for the messages to some topics, and `next()` waits for the next one:

```iris
import mqtt

!void main() {
    c := try mqtt.connect("broker.example.com", 1883, mqtt.Options{clientId: "kitchen-display"})
    defer c.close()
    _ = try c.subscribe("home/+/temp")
    try c.publish("home/kitchen/light", "on")
    for {
        m := try c.next()
        print("{m.topic}: {m.payload}")
        m.free()
    }
}
```

In a filter, `+` matches any one level of a topic and `#` matches any number of levels at the end. `home/+/temp` matches `home/kitchen/temp`, and `home/#` matches everything under `home`, and `home` itself. `mqtt.matches(filter, topic)` says whether a filter matches. `unsubscribe(filter)` stops a subscription.

`Options` has the rest of the login: `user` and `password`, `version` 4 for brokers that only speak 3.1.1, `tls` with `ca`, `cert` and `key` for port 8883, and `keepAlive`, how often a ping goes when nothing else does.

## Quality of service

Each message is sent at a quality of service:

- 0 is at most once. It is sent and not acknowledged, and it is lost if the connection goes down.
- 1 is at least once. `publish` returns once the broker has it, and it may arrive twice.
- 2 is exactly once. `publish` returns once the broker has it and knows it is not a repeat.

`publish(topic, payload, qos)` sends at a quality of service, and `subscribe(filter, qos)` asks for messages at most at that one. It gives the quality of service the broker grants. A message reaches a subscriber at the lower of the two.

## Retained messages and wills

`publish(topic, payload, qos, retain: true)` asks the broker to keep the message. A client that subscribes later gets it first, with `retain` set, so it learns the last state without waiting for the next change. An empty retained message removes the one kept.

A will is a message the broker publishes for a client when the client goes away without `close`, such as when its network drops. `willTopic`, `willMessage`, `willQos` and `willRetain` in `Options` set it:

```iris
c := try mqtt.connect("broker.example.com", 1883, mqtt.Options{clientId: "sensor-7",
    willTopic: "sensors/7/status", willMessage: "offline", willRetain: true})
try c.publish("sensors/7/status", "online", 1, true)
```

## Errors

The errors are `MqttError` codes:

- `connect`: the broker can't be reached.
- `refused`: the broker refused the login, a subscription or a message, with its reason.
- `timeout`: the broker didn't answer within `Options.timeout`, or no message came within the time given to `next`.
- `closed`: the connection is gone.

`alive()` says whether the connection is still up. Messages wait for `next` in a queue of `Options.queue` messages. When it is full, reading from the broker stops until `next` takes some.

## A broker

`mqtt.serve(port, broker)` runs a broker. An `mqtt.Broker` holds two functions that decide things. `login` takes a client id, a user and a password and says whether the client may connect. `allow` takes a user, a topic and whether it is a publish, and says whether the user may publish to it or subscribe to it. While they are `null`, everything is allowed. This program runs a broker and two clients:

```iris run
import mqtt, net

bool checkLogin(str id, str user, str password) => user == "ada" && password == "secret"

void serveIt(net.Listener ln) {
    mqtt.serveOn(ln, mqtt.Broker{login: checkLogin}) catch e { }
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    spawn serveIt(ln)
    login := mqtt.Options{user: "ada", password: "secret"}

    sensor := try mqtt.connect("127.0.0.1", ln.port, login)
    defer sensor.close()
    try sensor.publish("home/hall/temp", "19.5", 1, true)

    display := try mqtt.connect("127.0.0.1", ln.port, login)
    defer display.close()
    _ = try display.subscribe("home/#", 1)
    try sensor.publish("home/hall/temp", "20.0", 1)
    for i in 0..2 {
        m := try display.next()
        print("{m.topic} {m.payload} retained {m.retain}")
        m.free()
    }
    stranger := mqtt.connect("127.0.0.1", ln.port) catch e {
        print("no password: {e.code}")
        return
    }
    stranger.close()
}
```

```output
home/hall/temp 19.5 retained true
home/hall/temp 20.0 retained false
no password: MqttError.refused
```

The broker speaks MQTT 5 and 3.1.1 to its clients and passes messages between them. A session lasts while its client is connected: a client that comes back subscribes again. Retained messages are kept while the broker runs. A client that falls more than `Broker.queue` packets behind is dropped, so one slow client doesn't hold up the rest. The broker serves its connections on the thread that called `serve`.
