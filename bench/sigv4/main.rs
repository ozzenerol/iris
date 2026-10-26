use aws_credential_types::Credentials;
use aws_sigv4::http_request::{sign, PayloadChecksumKind, PercentEncodingMode, SignableBody, SignableRequest, SigningSettings, UriPathNormalizationMode};
use aws_sigv4::sign::v4;
use std::time::{Duration, UNIX_EPOCH};

// Signs 200000 S3 PUT requests, each with a 1 KiB body that is hashed, a
// Content-Type and its own key, at a fixed time, and prints how many
// signatures start with 0 and the last Authorization header.
fn main() {
    let identity = Credentials::new("AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", None, None, "bench").into();
    let mut settings = SigningSettings::default();
    settings.payload_checksum_kind = PayloadChecksumKind::XAmzSha256;
    settings.percent_encoding_mode = PercentEncodingMode::Single;
    settings.uri_path_normalization_mode = UriPathNormalizationMode::Disabled;
    let now = UNIX_EPOCH + Duration::from_secs(1792972800);
    let params: aws_sigv4::http_request::SigningParams = v4::SigningParams::builder()
        .identity(&identity)
        .region("eu-west-1")
        .name("s3")
        .time(now)
        .settings(settings)
        .build()
        .unwrap()
        .into();
    let body = "0123456789abcdef".repeat(64);
    let mut zeros = 0;
    let mut last = String::new();
    for i in 0..200000 {
        let url = format!("https://bench.s3.eu-west-1.amazonaws.com/photos/{}.txt", i);
        let req = SignableRequest::new("PUT", &url, [("content-type", "text/plain")].into_iter(), SignableBody::Bytes(body.as_bytes())).unwrap();
        let (instructions, _) = sign(req, &params).unwrap().into_parts();
        let (headers, _) = instructions.into_parts();
        let auth = headers.iter().find(|h| h.name() == "authorization").unwrap().value();
        if auth.as_bytes()[auth.len() - 64] == b'0' {
            zeros += 1;
        }
        if i == 199999 {
            last = format!("Authorization: {}", auth);
        }
    }
    println!("{} {}", zeros, last);
}
