//! ADB 无线配对 —— 纯 Rust 实现，零外部 adb 二进制依赖。
//!
//! 协议逐行移植 AOSP（pairing_connection / pairing_auth / boringssl spake25519）：
//!   1. 与手机配对端口建 TCP，TLS 1.3 双向自签证书（任意证书均接受）；
//!   2. 导出密钥材料（label = "adb-label\0"，NUL 含在长度内——与 BoringSSL
//!      sizeof(kExportedKeyLabel) 行为一致）拼接到配对码后作为 SPAKE2 口令；
//!   3. PairingPacket 帧（version/type/u32len）交换 SPAKE2 消息，推导
//!      AES-128-GCM 密钥（HKDF-SHA256，info="adb pairing_auth aes-128-gcm key"）；
//!   4. 加密交换 PeerInfo（本端发 adbkey 公钥行，对端回设备 GUID）。
//!
//! SPAKE2 细节（boringssl crypto/curve25519/spake25519.cc）：
//!   - ed25519 群；M/N 为固定点；password_hash = SHA512(pw)；
//!   - 口令标量 = sc_reduce(SHA512(pw))（mod L；BoringSSL 的 +kL 低 3 位 hack
//!     与 mod L 在点乘下等价，省略）；
//!   - 临时私钥 = sc_reduce(64B 随机) * 8（清 cofactor）；
//!   - my_msg = encode(x·B + w·(M|N))；共享密钥 K = SHA512 转录哈希：
//!     len64(my_name)||my_name||len64(their_name)||their_name||
//!     [alice: my_msg,their_msg | bob: their_msg,my_msg]（均带 len64 前缀）||
//!     len64(DH)||DH||len64(pw_hash)||pw_hash。
//!   - 角色名带 NUL：client "adb pair client\0"（16B，Alice）、
//!     server "adb pair server\0"（16B，Bob）——sizeof 包含终止符。

use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::OnceLock;
use std::time::Duration;

use aes_gcm::aead::{Aead, KeyInit};
use aes_gcm::{Aes128Gcm, Nonce};
use curve25519_dalek::constants::ED25519_BASEPOINT_TABLE;
use curve25519_dalek::edwards::{CompressedEdwardsY, EdwardsPoint};
use curve25519_dalek::scalar::Scalar;
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::pki_types::{CertificateDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::{ClientConfig, ClientConnection, DigitallySignedStruct, SignatureScheme};
use sha2::{Digest, Sha256, Sha512};

const CLIENT_NAME: &[u8] = b"adb pair client\0"; // sizeof 含 NUL = 16
const SERVER_NAME: &[u8] = b"adb pair server\0";
const TLS_EXPORT_LABEL: &[u8] = b"adb-label\0"; // sizeof 含 NUL = 10
const AES_KEY_INFO: &[u8] = b"adb pairing_auth aes-128-gcm key";
const PEER_INFO_SIZE: usize = 8192; // kMaxPeerInfoSize，整条加密交换
const PACKET_VERSION: u8 = 1;
const TYPE_SPAKE2_MSG: u8 = 0;
const TYPE_PEER_INFO: u8 = 1;
const PEER_INFO_RSA_PUB_KEY: u8 = 0;
const PEER_INFO_DEVICE_GUID: u8 = 1;

/// 64 字节随机源（/dev/urandom，跨平台无额外依赖）。
fn rand64() -> [u8; 64] {
    use std::io::Read as _;
    let mut buf = [0u8; 64];
    std::fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut buf))
        .expect("打开 /dev/urandom 失败");
    buf
}

// ---------- SPAKE2（boringssl spake25519 移植） ----------

/// N 点压缩编码（boringssl 注释中的 encoded 值）。
fn spake_n() -> &'static EdwardsPoint {
    static P: OnceLock<EdwardsPoint> = OnceLock::new();
    P.get_or_init(|| {
        CompressedEdwardsY([
            0x10, 0xe3, 0xdf, 0x0a, 0xe3, 0x7d, 0x8e, 0x7a, 0x99, 0xb5, 0xfe, 0x74, 0xb4,
            0x46, 0x72, 0x10, 0x3d, 0xbd, 0xdc, 0xbd, 0x06, 0xaf, 0x68, 0x0d, 0x71, 0x32,
            0x9a, 0x11, 0x69, 0x3b, 0xc7, 0x78,
        ])
        .decompress()
        .expect("SPAKE2 N 点")
    })
}

/// M 点压缩编码。
fn spake_m() -> &'static EdwardsPoint {
    static P: OnceLock<EdwardsPoint> = OnceLock::new();
    P.get_or_init(|| {
        CompressedEdwardsY([
            0x5a, 0xda, 0x7e, 0x4b, 0xf6, 0xdd, 0xd9, 0xad, 0xb6, 0x62, 0x6d, 0x32, 0x13,
            0x1c, 0x6b, 0x5c, 0x51, 0xa1, 0xe3, 0x47, 0xa3, 0x47, 0x8f, 0x53, 0xcf, 0xcf,
            0x44, 0x1b, 0x88, 0xee, 0xd1, 0x2e,
        ])
        .decompress()
        .expect("SPAKE2 M 点")
    })
}

#[derive(Clone, Copy, PartialEq)]
enum Role {
    Alice,
    Bob,
}

struct Spake2 {
    role: Role,
    my_name: &'static [u8],
    their_name: &'static [u8],
    password_hash: [u8; 64],
    password_scalar: Scalar,
    private_scalar: Scalar,
    my_msg: [u8; 32],
}

fn sc_reduce_wide(wide: &[u8; 64]) -> Scalar {
    Scalar::from_bytes_mod_order_wide(wide)
}

impl Spake2 {
    fn new(role: Role, password: &[u8]) -> Self {
        let (my_name, their_name) = match role {
            Role::Alice => (CLIENT_NAME, SERVER_NAME),
            Role::Bob => (SERVER_NAME, CLIENT_NAME),
        };
        // 临时私钥：64B 随机 → mod L → *8（BoringSSL left_shift_3）
        let mut priv_wide = rand64();
        let mut s = sc_reduce_wide(&priv_wide);
        priv_wide.zeroize();
        s = s * Scalar::from(8u64);
        let p = ED25519_BASEPOINT_TABLE * &s;

        let password_hash: [u8; 64] = Sha512::digest(password).into();
        // 口令标量：sc_reduce(SHA512(pw))；BoringSSL 的 +kL 低 3 位修正与 mod L 等价
        let password_scalar = sc_reduce_wide(&password_hash);

        let mask_point = if role == Role::Alice { spake_m() } else { spake_n() };
        let mask = password_scalar * mask_point;
        let p_star = (p + mask).compress().to_bytes();

        Spake2 {
            role,
            my_name,
            their_name,
            password_hash,
            password_scalar,
            private_scalar: s,
            my_msg: p_star,
        }
    }

    /// 处理对端消息，返回 64 字节共享密钥材料。
    fn process(&self, their_msg: &[u8]) -> Result<[u8; 64], String> {
        if their_msg.len() != 32 {
            return Err(format!("SPAKE2 消息长度非法: {}", their_msg.len()));
        }
        let q_star = CompressedEdwardsY(their_msg.try_into().unwrap())
            .decompress()
            .ok_or("对端 SPAKE2 点不在曲线上")?;

        let mask_point = if self.role == Role::Alice { spake_n() } else { spake_m() };
        let peers_mask = self.password_scalar * mask_point;
        let q_ext = q_star - peers_mask;
        let dh = (self.private_scalar * q_ext).compress().to_bytes();

        // 转录哈希（带 LE64 长度前缀）
        let mut sha = Sha512::new();
        let mut lp = |sha: &mut Sha512, data: &[u8]| {
            sha.update((data.len() as u64).to_le_bytes());
            sha.update(data);
        };
        match self.role {
            Role::Alice => {
                lp(&mut sha, self.my_name);
                lp(&mut sha, self.their_name);
                lp(&mut sha, &self.my_msg);
                lp(&mut sha, their_msg);
            }
            Role::Bob => {
                lp(&mut sha, self.their_name);
                lp(&mut sha, self.my_name);
                lp(&mut sha, their_msg);
                lp(&mut sha, &self.my_msg);
            }
        }
        lp(&mut sha, &dh);
        lp(&mut sha, &self.password_hash);
        Ok(sha.finalize().into())
    }
}

// Zeroize 手动兜底（临时私钥）。
trait Zeroize {
    fn zeroize(&mut self);
}
impl Zeroize for [u8; 64] {
    fn zeroize(&mut self) {
        for b in self.iter_mut() {
            *b = 0;
        }
    }
}

/// AES-128-GCM：HKDF-SHA256 派生 16B 密钥；nonce = LE32 序号 || 8 零字节；
/// 密文 = 明文 || 16B tag（与 EVP_AEAD_CTX_seal 一致）。
struct PairCipher {
    aead: Aes128Gcm,
    enc_seq: u64,
    dec_seq: u64,
}

impl PairCipher {
    fn new(key_material: &[u8; 64]) -> Self {
        let hk = hkdf::Hkdf::<Sha256>::new(None, key_material);
        let mut key = [0u8; 16];
        hk.expand(AES_KEY_INFO, &mut key).expect("HKDF aes-128 密钥");
        PairCipher {
            aead: Aes128Gcm::new_from_slice(&key).unwrap(),
            enc_seq: 0,
            dec_seq: 0,
        }
    }

    fn encrypt(&mut self, pt: &[u8]) -> Result<Vec<u8>, String> {
        // AOSP aes_128_gcm.cpp：nonce = LE64 序号 || 4 零字节
        let mut nonce = [0u8; 12];
        nonce[..8].copy_from_slice(&self.enc_seq.to_le_bytes());
        self.enc_seq += 1;
        self.aead
            .encrypt(Nonce::from_slice(&nonce), pt)
            .map_err(|e| format!("配对加密失败: {e}"))
    }

    fn decrypt(&mut self, ct: &[u8]) -> Result<Vec<u8>, String> {
        let mut nonce = [0u8; 12];
        nonce[..8].copy_from_slice(&self.dec_seq.to_le_bytes());
        self.dec_seq += 1;
        self.aead
            .decrypt(Nonce::from_slice(&nonce), ct)
            .map_err(|_| "配对解密失败（口令错误或密钥不匹配）".to_string())
    }
}

// ---------- TLS（rustls，接受任意自签证书） ----------

#[derive(Debug)]
struct AcceptAnyVerifier;

impl ServerCertVerifier for AcceptAnyVerifier {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Ok(HandshakeSignatureValid::assertion())
    }

    fn verify_tls13_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Ok(HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        vec![
            SignatureScheme::ECDSA_NISTP256_SHA256,
            SignatureScheme::ECDSA_NISTP384_SHA384,
            SignatureScheme::ED25519,
            SignatureScheme::RSA_PSS_SHA256,
            SignatureScheme::RSA_PSS_SHA384,
            SignatureScheme::RSA_PSS_SHA512,
            SignatureScheme::RSA_PKCS1_SHA256,
            SignatureScheme::RSA_PKCS1_SHA384,
            SignatureScheme::RSA_PKCS1_SHA512,
        ]
    }
}

/// 通过 KeyLog 捕获 EXPORTER_SECRET（rustls 不暴露导出主密钥，
/// 而 BoringSSL 的 TLS1.3 导出算法与 RFC 8448 不同，必须拿到原始密钥自行计算）。
/// 无线调试 TLS 连接（_adb._tcp）：用手机已信任的 adbkey 自签证书做
/// 客户端证书，与 adbd 的 TLS 服务端握手后即建立加密 ADB 通道。
/// 这是 Android 11+ 无线调试的标准连接方式，授权持久化无需重复弹窗。
pub fn tls_adb_connect(addr: &str) -> Result<(ClientConnection, TcpStream), String> {
    let host = addr.split(':').next().unwrap_or(addr).to_string();
    let sock_addr = addr.to_socket_addrs()
        .map_err(|e| format!("地址解析失败: {e}"))?
        .next()
        .ok_or("无可达地址")?;
    let mut stream = TcpStream::connect_timeout(&sock_addr, Duration::from_secs(5))
        .map_err(|e| format!("TLS 端口连接失败: {e}"))?;
    tls_wrap(stream, &host)
}

/// 对已建立的 TCP 连接做无线调试 TLS 升级（STLS 流程的第二阶段）。
pub fn tls_wrap(mut stream: TcpStream, host: &str) -> Result<(ClientConnection, TcpStream), String> {
    stream.set_nodelay(true).ok();
    stream.set_nonblocking(false).ok();

    // 用持久 adbkey 生成自签证书（无线调试服务端校验这个）
    let adbkey = crate::adbkeys::load_or_create();
    let (cert_der, key_der) = adbkey_self_signed_cert(&adbkey)?;

    let mut cfg = ClientConfig::builder()
        .with_root_certificates(rustls::RootCertStore::empty())
        .with_client_auth_cert(vec![cert_der], key_der.into())
        .map_err(|e| format!("TLS 客户端配置失败: {e}"))?;
    cfg.dangerous().set_certificate_verifier(std::sync::Arc::new(AcceptAnyVerifier));
    // 无线调试 ALPN：AOSP 用 "adb"；不设置则某些固件拒绝。若后续实测被设备拒，
    // 可试 vec![b"adb".to_vec(), b"adb-tls".to_vec()] 候选协商。
    cfg.alpn_protocols = vec![b"adb".to_vec()];

    let server_name = ServerName::try_from(host.to_string()).map_err(|e| format!("SNI 失败: {e}"))?;
    let mut tls = ClientConnection::new(std::sync::Arc::new(cfg), server_name)
        .map_err(|e| format!("TLS 初始化失败: {e}"))?;
    while tls.is_handshaking() {
        tls.complete_io(&mut stream).map_err(|e| format!("TLS 握手失败: {e}"))?;
    }
    Ok((tls, stream))
}

/// 用 adbkey（RSA-2048）生成自签 X.509 证书（无线调试客户端证书要求）
fn adbkey_self_signed_cert(key: &rsa::RsaPrivateKey) -> Result<(CertificateDer<'static>, PrivatePkcs8KeyDer<'static>), String> {
    use rsa::pkcs8::EncodePrivateKey;
    // rcgen 只支持 ECDSA/ED25519 直接生成；RSA 要走自定义 KeyPair
    let key_der = key.to_pkcs8_der().map_err(|e| format!("PKCS8 导出失败: {e}"))?;
    let key_der = PrivatePkcs8KeyDer::from(key_der.as_bytes().to_vec());
    let key_pair = rcgen::KeyPair::from_pkcs8_der_and_sign_algo(&key_der, &rcgen::PKCS_RSA_SHA256)
        .map_err(|e| format!("RSA KeyPair 构造失败: {e}"))?;
    let params = rcgen::CertificateParams::new(vec!["mwine-adb".to_string()])
        .map_err(|e| format!("证书参数失败: {e}"))?;
    let cert = params.self_signed(&key_pair).map_err(|e| format!("自签失败: {e}"))?;
    Ok((CertificateDer::from(cert.der().to_vec()), key_der))
}

fn make_client_config() -> Result<ClientConfig, String> {
    // 临时自签 ECDSA P-256 证书（配对专用，用完即弃）
    let key_pair = rcgen::KeyPair::generate_for(&rcgen::PKCS_ECDSA_P256_SHA256)
        .map_err(|e| format!("生成配对密钥失败: {e}"))?;
    let params = rcgen::CertificateParams::new(vec!["mwine-pairing".to_string()])
        .map_err(|e| format!("配对证书参数失败: {e}"))?;
    let cert = params
        .self_signed(&key_pair)
        .map_err(|e| format!("签发配对证书失败: {e}"))?;
    let cert_der = CertificateDer::from(cert.der().to_vec());
    let key_der = PrivatePkcs8KeyDer::from(key_pair.serialize_der());

    // 只提供 SHA-256 套件：实测部分固件（魅族 FlymeOS）配对码弹窗的服务端
    // 固定选 SHA384 套件、口令推导却按 SHA256 算——套件哈希与导出推导错位
    // 导致 SPAKE2 口令不一致（「配对解密失败」）。强制 SHA256 后两端一致；
    // 标准服务端（按协商哈希推导）不受影响。
    let mut provider = rustls::crypto::ring::default_provider();
    provider
        .cipher_suites
        .retain(|s| s.suite() == rustls::CipherSuite::TLS13_AES_128_GCM_SHA256);
    let mut cfg = ClientConfig::builder_with_provider(std::sync::Arc::new(provider))
        .with_safe_default_protocol_versions()
        .map_err(|e| format!("TLS 版本配置失败: {e}"))?
        .with_root_certificates(rustls::RootCertStore::empty())
        .with_client_auth_cert(vec![cert_der], key_der.into())
        .map_err(|e| format!("TLS 客户端配置失败: {e}"))?;
    cfg.dangerous()
        .set_certificate_verifier(std::sync::Arc::new(AcceptAnyVerifier));
    cfg.alpn_protocols = vec![b"adb".to_vec()];
    Ok(cfg)
}

// ---------- PairingPacket 帧 ----------

fn write_packet(tls: &mut ClientConnection, stream: &mut TcpStream, ty: u8, payload: &[u8]) -> Result<(), String> {
    let mut hdr = [0u8; 6];
    hdr[0] = PACKET_VERSION;
    hdr[1] = ty;
    hdr[2..6].copy_from_slice(&(payload.len() as u32).to_be_bytes());
    let mut buf = hdr.to_vec();
    buf.extend_from_slice(payload);
    tls.writer().write_all(&buf).map_err(|e| format!("TLS 写入失败: {e}"))?;
    tls.complete_io(stream).map_err(|e| format!("TLS 传输失败: {e}"))?;
    Ok(())
}

fn read_exact_tls(tls: &mut ClientConnection, stream: &mut TcpStream, n: usize) -> Result<Vec<u8>, String> {
    let mut out = vec![0u8; n];
    let mut filled = 0;
    // Stream::read 具备阻塞读语义：close_notify → Ok(0)＝真 EOF；
    // 读超时（EAGAIN/ETIMEDOUT）是"暂无数据"，继续等而不是当成对端关闭
    while filled < n {
        let mut st = rustls::Stream::new(&mut *tls, &mut *stream);
        match st.read(&mut out[filled..]) {
            Ok(0) => return Err("TLS 对端关闭".into()),
            Ok(k) => filled += k,
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) =>
            {
                std::thread::sleep(std::time::Duration::from_millis(20));
            }
            Err(e) => return Err(format!("TLS 读取失败: {e}")),
        }
    }
    Ok(out)
}

fn read_packet(tls: &mut ClientConnection, stream: &mut TcpStream, expect_ty: u8) -> Result<Vec<u8>, String> {
    let hdr = read_exact_tls(tls, stream, 6)?;
    if hdr[0] != PACKET_VERSION {
        return Err(format!("PairingPacket 版本不支持: {}", hdr[0]));
    }
    if hdr[1] != expect_ty {
        return Err(format!("PairingPacket 类型不符: 期望 {expect_ty} 实得 {}", hdr[1]));
    }
    let len = u32::from_be_bytes(hdr[2..6].try_into().unwrap()) as usize;
    if len == 0 || len > PEER_INFO_SIZE * 2 {
        return Err(format!("PairingPacket 载荷长度非法: {len}"));
    }
    read_exact_tls(tls, stream, len)
}

// ---------- 入口 ----------

/// 读取本机 adbkey 公钥行（$HOME/.android/adbkey.pub 首行）。
fn load_adbkey_pub() -> Result<Vec<u8>, String> {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/data".into());
    let path = format!("{home}/.android/adbkey.pub");
    let content = std::fs::read_to_string(&path)
        .map_err(|e| format!("读取 {path} 失败（需先启动 adb server 生成密钥）: {e}"))?;
    let line = content
        .lines()
        .next()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .ok_or_else(|| format!("{path} 为空"))?;
    Ok(line.as_bytes().to_vec())
}

/// 与手机配对端口执行完整配对流程。password = 配对码 || TLS1.3 导出密钥
/// （AOSP 语义：label "adb-label" 连 NUL 共 10B、长度 64；导出值与现行
/// BoringSSL/OpenSSL 一致，已用本地 OpenSSL 神谕与真机双向验证）。
/// 注意：客户端仅提供 SHA256 套件（实测部分固件配对码服务端固定选
/// SHA384 套件却按 SHA256 推导口令，套件受限后两端一致）。
/// 成功返回 paired:<GUID>（跨网段匹配连接服务用），手机此后信任本机 adbkey。
pub fn pair(addr: &str, code: &str) -> Result<String, String> {
    let host = addr
        .split(':')
        .next()
        .unwrap_or(addr)
        .to_string();
    let sock_addr = addr
        .to_socket_addrs()
        .map_err(|e| format!("配对地址解析失败: {e}"))?
        .next()
        .ok_or_else(|| format!("配对地址无效: {addr}"))?;
    let mut stream = TcpStream::connect_timeout(&sock_addr, Duration::from_secs(5))
        .map_err(|e| format!("连接配对端口 {addr} 失败: {e}（确认手机已打开无线调试配对弹窗）"))?;
    stream
        .set_read_timeout(Some(Duration::from_secs(15)))
        .ok();
    stream.set_nodelay(true).ok();
    // connect_timeout 可能残留非阻塞模式；读超时(EAGAIN)时 rustls 的
    // complete_io 会返回 Ok((n,0))，若不恢复阻塞会把 WouldBlock 误判成 EOF
    stream.set_nonblocking(false).ok();

    let server_name = ServerName::try_from(host)
        .map_err(|_| "配对地址非法".to_string())?;
    let cfg = make_client_config()?;
    let mut tls = ClientConnection::new(std::sync::Arc::new(cfg), server_name)
        .map_err(|e| format!("TLS 初始化失败: {e}"))?;
    while tls.is_handshaking() {
        tls.complete_io(&mut stream).map_err(|e| format!("TLS 握手失败: {e}"))?;
    }

    // 口令 = 配对码 || TLS1.3 导出密钥（64B）
    let mut exported = [0u8; 64];
    tls.export_keying_material(&mut exported, TLS_EXPORT_LABEL, None)
        .map_err(|e| format!("TLS 导出密钥失败: {e}"))?;
    let mut password = code.as_bytes().to_vec();
    password.extend_from_slice(&exported);

    // SPAKE2 消息交换（手机在校验失败/码过期时会直接关连接，报错给明确提示）
    let spake = Spake2::new(Role::Alice, &password);
    write_packet(&mut tls, &mut stream, TYPE_SPAKE2_MSG, &spake.my_msg)?;
    let their_msg = read_packet(&mut tls, &mut stream, TYPE_SPAKE2_MSG)
        .map_err(|e| format!("{e}（手机在校验阶段关闭了连接：配对码可能输错或已过期，请重新点「使用配对码配对设备」换新码重试）"))?;
    let key_material = spake.process(&their_msg)?;
    let mut cipher = PairCipher::new(&key_material);

    // PeerInfo 交换：本端发 adbkey 公钥行，对端回设备 GUID
    let pub_line = load_adbkey_pub()?;
    if pub_line.len() > PEER_INFO_SIZE - 1 {
        return Err("adbkey 公钥行过长".into());
    }
    let mut info = [0u8; PEER_INFO_SIZE];
    info[0] = PEER_INFO_RSA_PUB_KEY;
    info[1..1 + pub_line.len()].copy_from_slice(&pub_line);
    let enc = cipher.encrypt(&info)?;
    write_packet(&mut tls, &mut stream, TYPE_PEER_INFO, &enc)?;

    let their_enc = read_packet(&mut tls, &mut stream, TYPE_PEER_INFO)?;
    let their_plain = cipher.decrypt(&their_enc)?;
    if their_plain.len() != PEER_INFO_SIZE {
        return Err(format!("对端 PeerInfo 长度异常: {}", their_plain.len()));
    }
    let ty = their_plain[0];
    let guid = String::from_utf8_lossy(&their_plain[1..])
        .trim_end_matches('\0')
        .trim()
        .to_string();
    let _ = tls.send_close_notify();

    if ty == PEER_INFO_DEVICE_GUID {
        Ok(format!("paired:{guid}"))
    } else {
        Ok(format!("paired:peer_type={ty}"))
    }
}

// ---------- 测试 ----------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spake2_alice_bob_agree() {
        let password = b"749229";
        let alice = Spake2::new(Role::Alice, password);
        let bob = Spake2::new(Role::Bob, password);
        let ka = alice.process(&bob.my_msg).expect("alice process");
        let kb = bob.process(&alice.my_msg).expect("bob process");
        assert_eq!(ka, kb, "SPAKE2 双方共享密钥须一致");
    }

    #[test]
    fn spake2_wrong_password_differs() {
        let alice = Spake2::new(Role::Alice, b"111111");
        let bob = Spake2::new(Role::Bob, b"222222");
        let ka = alice.process(&bob.my_msg).unwrap();
        let kb = bob.process(&alice.my_msg).unwrap();
        assert_ne!(ka, kb);
    }

    #[test]
    fn cipher_roundtrip() {
        let km = [0x42u8; 64];
        let mut a = PairCipher::new(&km);
        let mut b = PairCipher::new(&km);
        let pt = [7u8; PEER_INFO_SIZE];
        let ct = a.encrypt(&pt).unwrap();
        assert_eq!(b.decrypt(&ct).unwrap(), pt.to_vec());
        // 序列号递增：第二条消息 nonce 不同
        let ct2 = a.encrypt(&pt).unwrap();
        assert_ne!(ct, ct2);
    }
}

#[cfg(test)]
mod live_tests {
    #[test]
    #[ignore = "真机抓包/联调：MWINE_PAIR_ADDR 与 MWINE_PAIR_CODE 指定配对地址（可指向本地代理）"]
    fn live_pair() {
        let addr = std::env::var("MWINE_PAIR_ADDR").expect("MWINE_PAIR_ADDR");
        let code = std::env::var("MWINE_PAIR_CODE").expect("MWINE_PAIR_CODE");
        let t0 = std::time::Instant::now();
        let r = super::pair(&addr, &code);
        eprintln!("pair({addr}) 用时 {:?} 结果: {r:?}", t0.elapsed());
    }
}
