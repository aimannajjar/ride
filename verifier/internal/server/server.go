package server

import (
	"bufio"
	"context"
	"crypto/rsa"
	"crypto/tls"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/quic-go/quic-go"
)

func VerifierServer(addr string, dbPath string) error {
	listener, err := quic.ListenAddr(addr, generateTLSConfig(), nil)
	if err != nil {
		panic(err)
	}
	defer listener.Close()

	fmt.Printf("Listening on %s", addr)

	conn, err := listener.Accept(context.Background())
	if err != nil {
		panic(err)
	}


	for {
		// TODO: off-load to a goroutine
		stream, err := conn.AcceptStream(context.Background())
		if err != nil {
			panic(err)
		}

		payload, err := io.ReadAll(stream)
		if err != nil {
			panic(err)
		}

		fmt.Printf("Received '%s'\n", string(payload))

		stream.Close()

	}

}

func generateTLSConfig() *tls.Config {
	// TODO: offer generating or specifying pre-existing
	cwd, err := os.Getwd()
	if err != nil {
		panic(err)
	}

	keyPath := filepath.Join(cwd, "../pki", "key.pem")
	certPath := filepath.Join(cwd, "../pki", "cert.pem")

	keyFile, err := os.Open(keyPath)
	if err != nil {
		panic(err)
	}

	keyReader := bufio.NewReader(keyFile)
	priv, err := rsa.GenerateKey(keyReader, 2048)

	certFile, err := os.Open(certPath)
	if err != nil {
		panic(err)
	}

	certReader := bufio.NewReader(certFile)
	certBytes, err := io.ReadAll(certReader)
	if err != nil {
		panic(err)
	}

	return &tls.Config{
		Certificates: []tls.Certificate{{
			Certificate: [][]byte{certBytes},
			PrivateKey:  priv,
		}},
	}

}
