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

	"github.com/aimannajjar/ride/verifier/internal/store"
	"github.com/quic-go/quic-go"
)

const hashLength = 32

type Server struct {
	addr   string
	dbPath string
	store  *store.Store
}

func NewServer(addr string, dbPath string) *Server {
	store := store.NewStore(dbPath)
	return &Server{
		addr:   addr,
		dbPath: dbPath,
		store:  store,
	}
}

func (s *Server) Run() {
	quicConfig := &quic.Config{MaxIdleTimeout: time.Duration(30) * time.Second,
		KeepAlivePeriod: time.Duration(15) * time.Second}

	listener, err := quic.ListenAddr(s.addr, generateTLSConfig(), quicConfig)
	if err != nil {
		panic(err)
	}
	defer listener.Close()

	fmt.Printf("Listening on %s\n", s.addr)

	for {
		conn, err := listener.Accept(context.Background())
		if err != nil {
			panic(err)
		}

		go s.handleClient(conn)
	}

}

func (s *Server) handleClient(conn *quic.Conn) {
	fmt.Println("Waiting for a stream")

	for {
		stream, err := conn.AcceptStream(context.Background())
		if err != nil {
			panic(err)
		}
		fmt.Printf("Accepted stream\n")

		payload, err := io.ReadAll(stream)
		if err != nil {
			panic(err)
		}

		hash := payload[:hashLength]
		filename := string(payload[hashLength:])

		fmt.Printf("Received hash for '%s'\n", filename)
		s.store.Set(filename, hash)
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
