package main

import (
	"flag"
	"fmt"

	"github.com/aimannajjar/ride/verifier/internal/server"
)

func usage() {
	fmt.Println("Usage: verifier [options]")
	flag.PrintDefaults()
}

func main() {
	flag.Usage = usage
	flag.Parse()

	dbPath := flag.String("db", "./store.db", "Location of verifier db (will create new one if it doesn't exist")

	args := flag.Args()
	if len(args) != 0 {
		usage()
	}

	server := server.NewServer("localhost:4433", *dbPath)

	// TODO: handle kill signals and do proper clean up
	server.Run()
}
