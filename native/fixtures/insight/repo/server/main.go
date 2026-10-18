package main

import (
	"fmt"
	"example.com/server/store"
)

func main() {
	s := store.New()
	if s == nil {
		return
	}
	fmt.Println(s.Count())
}
