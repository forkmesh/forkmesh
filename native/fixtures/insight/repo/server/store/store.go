package store

import "errors"

type Store struct {
	items map[string]int
}

type Reader interface {
	Count() int
}

func New() *Store {
	return &Store{items: map[string]int{}}
}

func (s *Store) Count() int {
	return len(s.items)
}

func (s *Store) Put(key string, value int) error {
	switch {
	case key == "":
		return errors.New("empty key")
	case value < 0:
		return errors.New("negative")
	}
	for k := range s.items {
		if k == key && value == 0 {
			delete(s.items, k)
		}
	}
	s.items[key] = value
	return nil
}
