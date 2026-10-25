use ring::rand::SystemRandom;
use ring::signature::{EcdsaKeyPair, KeyPair, UnparsedPublicKey, ECDSA_P256_SHA256_ASN1, ECDSA_P256_SHA256_ASN1_SIGNING};

const KEY: &str = "308187020100301306072a8648ce3d020106082a8648ce3d030107046d306b0201010420c1ac5e74c316ff54dfd13d0a034956ba1b5b656e892a670669beb1af9f5a8af8a1440342000411553b572004d7ab330c2cab9855252d50dcc8ba9ef9c16918af6bfd93e18f2ce061f3467cb60de84632be7425c9f41453f606fe372bf6c2a89c5f603398cb1e";

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

// 4000 messages signed with ECDSA P-256 and SHA-256, each signature checked.
fn main() {
    let rng = SystemRandom::new();
    let key = EcdsaKeyPair::from_pkcs8(&ECDSA_P256_SHA256_ASN1_SIGNING, &unhex(KEY), &rng).unwrap();
    let public = UnparsedPublicKey::new(&ECDSA_P256_SHA256_ASN1, key.public_key().as_ref().to_vec());
    let mut ok = 0;
    for i in 0..4000 {
        let msg = format!("message {} for the signature benchmark", i);
        let sig = key.sign(&rng, msg.as_bytes()).unwrap();
        if public.verify(msg.as_bytes(), sig.as_ref()).is_ok() {
            ok += 1;
        }
    }
    println!("{}", ok);
}
