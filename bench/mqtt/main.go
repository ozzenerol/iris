package main

// An MQTT broker, a publisher and a subscriber in one program, over
// loopback: 200000 messages of 64 bytes at QoS 0, then 20000 at QoS 1,
// each waited for by the publisher, all through the broker to the
// subscriber. mochi-mqtt's broker and Eclipse Paho's client.

import (
	"fmt"
	"io"
	"log/slog"
	"net"

	paho "github.com/eclipse/paho.mqtt.golang"
	mqtt "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/hooks/auth"
	"github.com/mochi-mqtt/server/v2/listeners"
)

const fast = 200000
const sure = 20000

func main() {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	addr := ln.Addr().String()
	ln.Close()
	// mochi drops QoS 0 messages for a subscriber whose queue is full;
	// room for all of them keeps the count the same as the others'
	caps := mqtt.NewDefaultServerCapabilities()
	caps.MaximumClientWritesPending = fast + sure
	server := mqtt.New(&mqtt.Options{Capabilities: caps, Logger: slog.New(slog.NewTextHandler(io.Discard, nil))})
	server.AddHook(new(auth.AllowHook), nil)
	if err := server.AddListener(listeners.NewTCP(listeners.Config{ID: "t", Address: addr})); err != nil {
		panic(err)
	}
	go server.Serve()
	defer server.Close()

	done := make(chan int)
	got, bytes := 0, 0
	subOpts := paho.NewClientOptions().AddBroker("tcp://" + addr).SetClientID("sub").SetOrderMatters(true)
	sub := paho.NewClient(subOpts)
	if t := sub.Connect(); t.Wait() && t.Error() != nil {
		panic(t.Error())
	}
	t := sub.Subscribe("bench/#", 1, func(c paho.Client, m paho.Message) {
		got++
		bytes += len(m.Payload())
		if got == fast+sure {
			done <- bytes
		}
	})
	t.Wait()
	pubOpts := paho.NewClientOptions().AddBroker("tcp://" + addr).SetClientID("pub")
	pub := paho.NewClient(pubOpts)
	if t := pub.Connect(); t.Wait() && t.Error() != nil {
		panic(t.Error())
	}
	payload := make([]byte, 64)
	for i := range payload {
		payload[i] = byte('a' + i%26)
	}
	for i := 0; i < fast; i++ {
		pub.Publish("bench/fast", 0, false, payload)
	}
	for i := 0; i < sure; i++ {
		pub.Publish("bench/sure", 1, false, payload).Wait()
	}
	n := <-done
	fmt.Printf("messages %d, bytes %d\n", fast+sure, n)
	pub.Disconnect(0)
	sub.Disconnect(0)
}
