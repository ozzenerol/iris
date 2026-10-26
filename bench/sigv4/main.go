package main

// Signs 200000 S3 PUT requests, each with a 1 KiB body that is hashed, a
// Content-Type and its own key, at a fixed time, and prints how many
// signatures start with 0 and the last Authorization header.

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/aws/aws-sdk-go-v2/aws"
	v4 "github.com/aws/aws-sdk-go-v2/aws/signer/v4"
)

func main() {
	creds := aws.Credentials{AccessKeyID: "AKIAIOSFODNN7EXAMPLE", SecretAccessKey: "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"}
	signer := v4.NewSigner(func(o *v4.SignerOptions) { o.DisableURIPathEscaping = true })
	body := strings.Repeat("0123456789abcdef", 64)
	now := time.Date(2026, 10, 26, 0, 0, 0, 0, time.UTC)
	ctx := context.Background()
	zeros := 0
	last := ""
	for i := 0; i < 200000; i++ {
		url := fmt.Sprintf("https://bench.s3.eu-west-1.amazonaws.com/photos/%d.txt", i)
		req, _ := http.NewRequest("PUT", url, nil)
		req.Header.Set("Content-Type", "text/plain")
		sum := sha256.Sum256([]byte(body))
		hash := hex.EncodeToString(sum[:])
		req.Header.Set("X-Amz-Content-Sha256", hash)
		if err := signer.SignHTTP(ctx, creds, req, hash, "s3", "eu-west-1", now); err != nil {
			panic(err)
		}
		auth := req.Header.Get("Authorization")
		if auth[len(auth)-64] == '0' {
			zeros++
		}
		if i == 199999 {
			last = "Authorization: " + auth
		}
	}
	fmt.Println(zeros, last)
}
