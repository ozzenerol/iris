use x509_parser::prelude::*;

const CERT: &str = "30820250308201f6a00302010202144392617033f0b534f859af149e0297183f07ec38300a06082a8648ce3d0403023041310b300906035504061302524f31163014060355040a0c0d4b7265797a6f6e2042656e6368311a301806035504030c1162656e63682e6578616d706c652e636f6d301e170d3236313032353136313233385a170d3336313032323136313233385a3041310b300906035504061302524f31163014060355040a0c0d4b7265797a6f6e2042656e6368311a301806035504030c1162656e63682e6578616d706c652e636f6d3059301306072a8648ce3d020106082a8648ce3d0301070342000411553b572004d7ab330c2cab9855252d50dcc8ba9ef9c16918af6bfd93e18f2ce061f3467cb60de84632be7425c9f41453f606fe372bf6c2a89c5f603398cb1ea381cb3081c8301d0603551d0e041604145742d28738f92d47a143183cb8ae4e8e336485e9301f0603551d230418301680145742d28738f92d47a143183cb8ae4e8e336485e9300f0603551d130101ff040530030101ff30500603551d1104493047821162656e63682e6578616d706c652e636f6d82157777772e62656e63682e6578616d706c652e636f6d82156170692e62656e63682e6578616d706c652e636f6d87040a000001300e0603551d0f0101ff04040302078030130603551d25040c300a06082b06010505070301300a06082a8648ce3d0403020348003045022016d326b8d8889133af9a52b8653e4dac69e58d53e295f697dac9930c7ba39b38022100ff14583ea7306982a5de7573800b293cb3bb3bddc7f438b354484a0888f48975";

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

// A certificate parsed 200000 times, its names and dates read each time.
fn main() {
    let der = unhex(CERT);
    let mut total = 0i64;
    for _ in 0..200000 {
        let (_, c) = X509Certificate::from_der(&der).unwrap();
        let (mut dns, mut ips) = (0, 0);
        if let Ok(Some(san)) = c.subject_alternative_name() {
            for n in &san.value.general_names {
                match n {
                    GeneralName::DNSName(_) => dns += 1,
                    GeneralName::IPAddress(_) => ips += 1,
                    _ => {}
                }
            }
        }
        let cn = c.subject().iter_common_name().next().and_then(|a| a.as_str().ok()).unwrap_or("");
        total += (dns + ips + cn.len()) as i64;
        total += c.validity().not_after.timestamp() - c.validity().not_before.timestamp();
    }
    println!("{}", total);
}
