package store

import (
	"encoding/hex"
	"fmt"
)

type Store struct {
	path string
}

func NewStore(path string) *Store {
	fmt.Println("You're tryin to load a store, that's a good first step")

	return &Store{
		path: path,
	}
}


func (s *Store) Set(key string, val []byte) {
	fmt.Printf("Setting %s to %s\n", key, hex.EncodeToString(val));
}
