package main

import (
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/hex"
	"fmt"
)

const key = "308187020100301306072a8648ce3d020106082a8648ce3d030107046d306b0201010420c1ac5e74c316ff54dfd13d0a034956ba1b5b656e892a670669beb1af9f5a8af8a1440342000411553b572004d7ab330c2cab9855252d50dcc8ba9ef9c16918af6bfd93e18f2ce061f3467cb60de84632be7425c9f41453f606fe372bf6c2a89c5f603398cb1e"

// 4000 messages signed with ECDSA P-256 and SHA-256, each signature checked.
func main() {
	der, _ := hex.DecodeString(key)
	k, err := x509.ParsePKCS8PrivateKey(der)
	if err != nil {
		panic(err)
	}
	priv := k.(*ecdsa.PrivateKey)
	ok := 0
	for i := 0; i < 4000; i++ {
		msg := fmt.Sprintf("message %d for the signature benchmark", i)
		h := sha256.Sum256([]byte(msg))
		sig, err := ecdsa.SignASN1(rand.Reader, priv, h[:])
		if err != nil {
			panic(err)
		}
		if ecdsa.VerifyASN1(&priv.PublicKey, h[:], sig) {
			ok++
		}
	}
	fmt.Println(ok)
}
