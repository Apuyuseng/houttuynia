//! 用本地 mock smart-socket 服务器验证客户端协议实现的正确性。

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc::{channel, Receiver};
use std::thread;

use mwine_adb::AdbClient;

/// 启动一个支持本测试所需服务子集的 mock adb server。
fn spawn_mock() -> (u16, Receiver<MockEvent>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let (tx, rx) = channel();
    thread::spawn(move || {
        for stream in listener.incoming() {
            let Ok(stream) = stream else { break };
            let tx = tx.clone();
            thread::spawn(move || handle_conn(stream, tx));
        }
    });
    (port, rx)
}

#[derive(Debug, Clone)]
enum MockEvent {
    SyncPush { path: String, bytes: Vec<u8> },
}

fn read_exact_n(stream: &mut TcpStream, n: usize) -> Vec<u8> {
    let mut buf = vec![0u8; n];
    stream.read_exact(&mut buf).unwrap();
    buf
}

fn read_request(stream: &mut TcpStream) -> String {
    let hex = read_exact_n(stream, 4);
    let len = usize::from_str_radix(std::str::from_utf8(&hex).unwrap(), 16).unwrap();
    String::from_utf8(read_exact_n(stream, len)).unwrap()
}

fn write_status(stream: &mut TcpStream, s: &str) {
    stream.write_all(s.as_bytes()).unwrap();
}

fn write_payload(stream: &mut TcpStream, data: &[u8]) {
    stream
        .write_all(format!("{:04x}", data.len()).as_bytes())
        .unwrap();
    stream.write_all(data).unwrap();
}

fn handle_conn(mut stream: TcpStream, tx: std::sync::mpsc::Sender<MockEvent>) {
    loop {
        let service = read_request(&mut stream);
        match service.as_str() {
            "host:version" => {
                write_status(&mut stream, "OKAY");
                write_payload(&mut stream, b"0024");
            }
            "host:devices" => {
                write_status(&mut stream, "OKAY");
                write_payload(&mut stream, b"emulator-5554\tdevice\n");
            }
            s if s.starts_with("host:transport:") => {
                write_status(&mut stream, "OKAY");
                // transport 之后同一连接上还会跟一个服务请求；
                // 该请求结束后连接即由客户端关闭
                let next = read_request(&mut stream);
                handle_transported(&mut stream, &next, &tx);
                return;
            }
            s if s.starts_with("host:forward:")
                || s.starts_with("host:remove-forward:")
                || s.starts_with("host-serial:") => {
                write_status(&mut stream, "OKAY");
            }
            _ => {
                write_status(&mut stream, "FAIL");
                write_payload(
                    &mut stream,
                    format!("unknown service: {service}").as_bytes(),
                );
                return;
            }
        }
    }
}

fn handle_transported(
    stream: &mut TcpStream,
    service: &str,
    tx: &std::sync::mpsc::Sender<MockEvent>,
) {
    if service == "sync:" {
        write_status(stream, "OKAY");
        handle_sync(stream, tx);
    } else if service.starts_with("reverse:") {
        // reverse:forward / reverse:killforward：均以 OKAY 起头
        //（forward 之后真实 adb 还会回显端口，客户端不读取，这里省略）
        write_status(stream, "OKAY");
    } else if let Some(cmd) = service.strip_prefix("shell,v2,raw:") {
        write_status(stream, "OKAY");
        if cmd == "echo hello" {
            let data = b"hello\n";
            stream.write_all(&[0x1]).unwrap();
            stream
                .write_all(&(data.len() as u32).to_le_bytes())
                .unwrap();
            stream.write_all(data).unwrap();
        } else if cmd.starts_with("pm install") {
            stream.write_all(&[0x1]).unwrap();
            stream.write_all(&8u32.to_le_bytes()).unwrap();
            stream.write_all(b"Success\n").unwrap();
        }
        // exit chunk = 0
        stream.write_all(&[0x3]).unwrap();
        stream.write_all(&1u32.to_le_bytes()).unwrap();
        stream.write_all(&[0]).unwrap();
    } else if let Some(cmd) = service.strip_prefix("shell:") {
        write_status(stream, "OKAY");
        if cmd.starts_with("pm install") {
            stream.write_all(b"Success\n").unwrap();
        }
    } else {
        write_status(stream, "FAIL");
        write_payload(stream, b"unsupported transported service");
    }
}

/// mock 设备上可供拉取的虚拟文件。
const MOCK_PULL_BYTES: &[u8] = b"PK\x03\x04fake apk bytes \x00\x01";

fn handle_sync(stream: &mut TcpStream, tx: &std::sync::mpsc::Sender<MockEvent>) {
    // 读到 QUIT 或连接关闭为止；解析 SEND/DATA/DONE、STAT/RECV
    let mut path = String::new();
    let mut data = Vec::new();
    loop {
        let cmd = read_exact_n(stream, 4);
        let len = u32::from_le_bytes(read_exact_n(stream, 4)[..].try_into().unwrap()) as usize;
        match cmd.as_slice() {
            b"SEND" => {
                path = String::from_utf8(read_exact_n(stream, len)).unwrap();
                // "path,mode" 格式
                if let Some(idx) = path.rfind(',') {
                    path.truncate(idx);
                }
            }
            b"DATA" => {
                data.extend_from_slice(&read_exact_n(stream, len));
            }
            b"DONE" => {
                read_exact_n(stream, len); // mtime
                                           // 真实 adb 的 sync 回复固定 8 字节：4 字节状态 + 4 字节长度
                stream.write_all(b"OKAY").unwrap();
                stream.write_all(&0u32.to_le_bytes()).unwrap();
                tx.send(MockEvent::SyncPush {
                    path: path.clone(),
                    bytes: data.clone(),
                })
                .unwrap();
                data.clear();
            }
            b"STAT" => {
                read_exact_n(stream, len); // path
                let mut stat = Vec::new();
                stat.extend_from_slice(b"STAT");
                stat.extend_from_slice(&33_206u32.to_le_bytes()); // mode
                stat.extend_from_slice(&(MOCK_PULL_BYTES.len() as u32).to_le_bytes());
                stat.extend_from_slice(&0u32.to_le_bytes()); // mtime
                stream.write_all(&stat).unwrap();
            }
            b"RECV" => {
                read_exact_n(stream, len); // path
                for chunk in MOCK_PULL_BYTES.chunks(8) {
                    stream.write_all(b"DATA").unwrap();
                    stream
                        .write_all(&(chunk.len() as u32).to_le_bytes())
                        .unwrap();
                    stream.write_all(chunk).unwrap();
                }
                stream.write_all(b"DONE").unwrap();
                stream.write_all(&0u32.to_le_bytes()).unwrap();
            }
            b"QUIT" => return,
            _ => {
                write_status(stream, "FAIL");
                write_payload(stream, b"unexpected sync request");
                return;
            }
        }
    }
}

fn client_for(port: u16) -> AdbClient {
    AdbClient {
        host: "127.0.0.1".into(),
        port,
    }
}

const SERIAL: &str = "emulator-5554";

#[test]
fn version_round_trip() {
    let (port, _rx) = spawn_mock();
    let client = client_for(port);
    assert_eq!(client.version().unwrap(), "0024");
}

#[test]
fn devices_parse() {
    let (port, _rx) = spawn_mock();
    let client = client_for(port);
    let devices = client.devices().unwrap();
    assert_eq!(devices.len(), 1);
    assert_eq!(devices[0].serial, SERIAL);
    assert_eq!(devices[0].state, "device");
}

#[test]
fn shell_v2_output_and_exit_code() {
    let (port, _rx) = spawn_mock();
    let client = client_for(port);
    let out = client
        .shell_blocking(SERIAL, "echo hello", std::time::Duration::from_secs(5))
        .unwrap();
    assert_eq!(out.stdout_string(), "hello\n");
    assert_eq!(out.exit_code, Some(0));
}

#[test]
fn push_bytes_round_trip_via_sync() {
    let (port, rx) = spawn_mock();
    let client = client_for(port);
    let payload = b"fake jar bytes \x00\x01\x02".to_vec();
    client
        .push_bytes(SERIAL, &payload, "/data/local/tmp/scrcpy-server")
        .unwrap();
    let event = rx.recv_timeout(std::time::Duration::from_secs(5)).unwrap();
    match event {
        MockEvent::SyncPush { path, bytes } => {
            assert_eq!(path, "/data/local/tmp/scrcpy-server");
            assert_eq!(bytes, payload);
        }
    }
}

#[test]
fn forward_and_remove() {
    let (port, _rx) = spawn_mock();
    let client = client_for(port);
    client.forward_abstract(SERIAL, 27183, "scrcpy").unwrap();
    client.remove_forward(SERIAL, 27183).unwrap();
}

#[test]
fn reverse_install_and_remove() {
    let (port, _rx) = spawn_mock();
    let client = client_for(port);
    client.reverse_tcp(SERIAL, 27184, 27184).unwrap();
    client.remove_reverse(SERIAL, 27184).unwrap();
}

#[test]
fn pull_file_round_trip_via_sync() {
    let (port, _rx) = spawn_mock();
    let client = client_for(port);
    let data = client
        .pull_file(SERIAL, "/data/app/~~demo==/base.apk")
        .unwrap();
    assert_eq!(data, MOCK_PULL_BYTES);
}

#[test]
fn install_flow_reports_success() {
    let (port, _rx) = spawn_mock();
    let client = client_for(port);
    // 写一个假 apk 文件走完整 install 链路（push + pm install）
    let dir = std::env::temp_dir().join("mwine-adb-test");
    std::fs::create_dir_all(&dir).unwrap();
    let apk = dir.join("demo.apk");
    std::fs::write(&apk, b"PK\x03\x04fake").unwrap();
    let out = client
        .install_apk(SERIAL, &apk, std::time::Duration::from_secs(5))
        .unwrap();
    assert!(out.contains("Success"));
}
