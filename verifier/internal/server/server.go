package server

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"io"
	"math/big"
	"time"

	"github.com/quic-go/quic-go"
)

func VerifierServer(addr string, dbPath string) error {
	quicConfig := &quic.Config{MaxIdleTimeout: time.Duration(30) * time.Second,
		KeepAlivePeriod: time.Duration(15) * time.Second}

	listener, err := quic.ListenAddr(addr, generateTLSConfig(), quicConfig)
	if err != nil {
		panic(err)
	}
	defer listener.Close()

	fmt.Printf("Listening on %s\n", addr)

	conn, err := listener.Accept(context.Background())
	if err != nil {
		panic(err)
	}

	for {
		// TODO: off-load to a goroutine
		fmt.Println("Waiting for a stream");
		stream, err := conn.AcceptStream(context.Background())
		if err != nil {
			panic(err)
		}
		fmt.Printf("Accepted stream\n");

		payload, err := io.ReadAll(stream)
		if err != nil {
			panic(err)
		}

		fmt.Printf("Received '%s'\n", string(payload))
		stream.Close()
	}

}
func generateTLSConfig() *tls.Config {
	priv, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		panic(err)
	}
	template := x509.Certificate{SerialNumber: big.NewInt(1)}
	certDER, err := x509.CreateCertificate(rand.Reader, &template, &template, priv.Public(), priv)
	if err != nil {
		panic(err)
	}

	return &tls.Config{
		Certificates: []tls.Certificate{{
			Certificate: [][]byte{certDER},
			PrivateKey:  priv,
		}},
		NextProtos: []string{"ride0.1"},
	}
}
