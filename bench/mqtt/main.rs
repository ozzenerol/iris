// An MQTT broker, a publisher and a subscriber in one program, over
// loopback: 200000 messages of 64 bytes at QoS 0, then 20000 at QoS 1,
// each waited for by the publisher, all through the broker to the
// subscriber. rumqttd's broker on threads of its own and rumqttc's client
// on the multi-threaded runtime.

use rumqttc::{AsyncClient, Event, MqttOptions, Packet, QoS};
use std::time::Duration;
use tokio::sync::mpsc;

const FAST: usize = 200000;
const SURE: usize = 20000;

fn broker_config(port: u16) -> rumqttd::Config {
    let text = format!(
        r#"
id = 0
[router]
id = 0
max_connections = 100
max_outgoing_packet_count = 200
max_segment_size = 104857600
max_segment_count = 10

[v4.1]
name = "v4-1"
listen = "127.0.0.1:{port}"
next_connection_delay_ms = 0
    [v4.1.connections]
    connection_timeout_ms = 60000
    max_payload_size = 20480
    max_inflight_count = 100
    dynamic_filters = true
"#
    );
    toml::from_str(&text).unwrap()
}

#[tokio::main]
async fn main() {
    let port = std::net::TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
    let mut broker = rumqttd::Broker::new(broker_config(port));
    std::thread::spawn(move || broker.start().unwrap());
    // the broker listens once its thread is up
    loop {
        if std::net::TcpStream::connect(("127.0.0.1", port)).is_ok() {
            break;
        }
        std::thread::sleep(Duration::from_millis(5));
    }

    let mut so = MqttOptions::new("sub", "127.0.0.1", port);
    so.set_keep_alive(Duration::from_secs(60));
    let (sub, mut sub_loop) = AsyncClient::new(so, 1 << 16);
    let (subscribed_tx, mut subscribed) = mpsc::channel(1);
    let (done_tx, mut done) = mpsc::channel(1);
    tokio::spawn(async move {
        let (mut got, mut bytes) = (0, 0);
        loop {
            match sub_loop.poll().await.unwrap() {
                Event::Incoming(Packet::SubAck(_)) => subscribed_tx.send(()).await.unwrap(),
                Event::Incoming(Packet::Publish(p)) => {
                    got += 1;
                    bytes += p.payload.len();
                    if got == FAST + SURE {
                        done_tx.send(bytes).await.unwrap();
                    }
                }
                _ => {}
            }
        }
    });
    sub.subscribe("bench/#", QoS::AtLeastOnce).await.unwrap();
    subscribed.recv().await.unwrap();

    let mut po = MqttOptions::new("pub", "127.0.0.1", port);
    po.set_keep_alive(Duration::from_secs(60));
    let (pub_client, mut pub_loop) = AsyncClient::new(po, 1 << 16);
    let (acked_tx, mut acked) = mpsc::channel(1);
    tokio::spawn(async move {
        loop {
            if let Event::Incoming(Packet::PubAck(_)) = pub_loop.poll().await.unwrap() {
                acked_tx.send(()).await.unwrap();
            }
        }
    });
    let payload: Vec<u8> = (0..64).map(|i| b'a' + (i % 26) as u8).collect();
    for _ in 0..FAST {
        pub_client.publish("bench/fast", QoS::AtMostOnce, false, payload.clone()).await.unwrap();
    }
    for _ in 0..SURE {
        pub_client.publish("bench/sure", QoS::AtLeastOnce, false, payload.clone()).await.unwrap();
        acked.recv().await.unwrap();
    }
    let bytes = done.recv().await.unwrap();
    println!("messages {}, bytes {}", FAST + SURE, bytes);
    std::process::exit(0);
}
