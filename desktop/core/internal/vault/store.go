package vault

import (
	"errors"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

type Store struct {
	Path    string
	Signers func() []Signer
	Now     func() time.Time

	mu sync.Mutex
}

type Status struct {
	State      string   `json:"state"`
	Path       string   `json:"path"`
	KeyID      string   `json:"keyId,omitempty"`
	Generation int64    `json:"generation"`
	Count      int      `json:"count"`
	Identities []string `json:"identities,omitempty"`
}

type Tx struct {
	doc   *document
	dek   []byte
	now   int64
	dirty bool
	set   map[string]bool
}

func (t *Tx) Names(prefix string) []string {
	out := []string{}
	for _, name := range sortedNames(t.doc.Entries) {
		if strings.HasPrefix(name, prefix) {
			out = append(out, name)
		}
	}
	return out
}

func (t *Tx) Has(name string) bool {
	_, ok := t.doc.Entries[name]
	return ok
}

func (t *Tx) At(name string) int64 { return t.doc.Entries[name].At }

func (t *Tx) Get(name string) ([]byte, bool, error) {
	entry, ok := t.doc.Entries[name]
	if !ok {
		return nil, false, nil
	}
	plain, err := openEntry(t.dek, t.doc.KeyID, name, entry)
	if err != nil {
		return nil, true, err
	}
	return plain, true, nil
}

func (t *Tx) Set(name string, value []byte) error {
	if !ValidName(name) {
		return ErrInvalidName
	}
	if len(value) > MaxValueBytes {
		return ErrTooLarge
	}
	if _, ok := t.doc.Entries[name]; !ok && len(t.doc.Entries) >= MaxEntries {
		return ErrTooLarge
	}
	t.doc.Entries[name] = sealEntry(t.dek, t.doc.KeyID, name, value, t.now)
	t.dirty = true
	if t.set == nil {
		t.set = map[string]bool{}
	}
	t.set[name] = true
	return nil
}

func (t *Tx) Delete(name string) bool {
	if _, ok := t.doc.Entries[name]; !ok {
		return false
	}
	delete(t.doc.Entries, name)
	t.dirty = true
	return true
}

func (s *Store) now() time.Time {
	if s.Now != nil {
		return s.Now()
	}
	return time.Now()
}

func (s *Store) signers() []Signer {
	if s.Signers == nil {
		return nil
	}
	out := []Signer{}
	for _, signer := range s.Signers() {
		if signer != nil && signer.PublicKey() != "" {
			out = append(out, signer)
		}
	}
	return out
}

func (s *Store) Exists() bool {
	info, err := os.Lstat(s.Path)
	return err == nil && info.Mode().IsRegular()
}

func (s *Store) read() (*document, error) {
	info, err := os.Lstat(s.Path)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() > MaxFileBytes {
		return nil, ErrFormat
	}
	file, err := os.Open(s.Path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	raw, err := io.ReadAll(io.LimitReader(file, MaxFileBytes+1))
	if err != nil {
		return nil, err
	}
	return parseDocument(raw)
}

func (s *Store) unlock(doc *document) ([]byte, bool, error) {
	signers := s.signers()
	if len(signers) == 0 {
		return nil, false, ErrNoIdentity
	}
	current := signers[0].PublicKey()
	currentWrapped := false
	for _, wrap := range doc.Wraps {
		if wrap.Identity == current {
			currentWrapped = true
		}
	}
	matched := false
	for _, signer := range signers {
		for _, wrap := range doc.Wraps {
			if wrap.Identity != signer.PublicKey() {
				continue
			}
			matched = true
			dek, err := unwrapKey(signer, wrap, doc.KeyID)
			if err != nil {
				continue
			}
			if !verifyMAC(dek, doc) {
				zero(dek)
				return nil, false, ErrTampered
			}
			return dek, !currentWrapped, nil
		}
	}
	if matched {
		return nil, false, ErrTampered
	}
	return nil, false, ErrIdentityMismatch
}

func (s *Store) addWrap(doc *document, dek []byte) {
	signers := s.signers()
	if len(signers) == 0 {
		return
	}
	wraps := []wrapRecord{wrapKey(signers[0], dek, doc.KeyID)}
	for _, wrap := range doc.Wraps {
		if wrap.Identity != signers[0].PublicKey() && len(wraps) < maxWraps {
			wraps = append(wraps, wrap)
		}
	}
	doc.Wraps = wraps
}

func (s *Store) create() (*document, []byte, error) {
	signers := s.signers()
	if len(signers) == 0 {
		return nil, nil, ErrNoIdentity
	}
	dek := randomBytes(keyBytes)
	doc := &document{Kind: Kind, V: Version, KeyID: keyIDOf(dek), Entries: map[string]entryRecord{}}
	doc.Wraps = []wrapRecord{wrapKey(signers[0], dek, doc.KeyID)}
	return doc, dek, nil
}

func (s *Store) save(doc *document, dek []byte) error {
	doc.Generation++
	doc.MAC = macOf(dek, doc)
	raw, err := marshalDocument(doc)
	if err != nil {
		return err
	}
	if len(raw) > MaxFileBytes {
		return ErrTooLarge
	}
	return writeAtomic(s.Path, raw)
}

func (s *Store) View(fn func(tx *Tx) error) error {
	return s.run(false, fn)
}

func (s *Store) Update(fn func(tx *Tx) error) error {
	return s.run(true, fn)
}

func (s *Store) run(create bool, fn func(tx *Tx) error) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	doc, err := s.read()
	if err != nil {
		return err
	}
	var dek []byte
	rewrap := false
	if doc == nil {
		if !create {
			empty := &document{Kind: Kind, V: Version, Entries: map[string]entryRecord{}}
			return fn(&Tx{doc: empty, now: s.now().UnixMilli()})
		}
		doc, dek, err = s.create()
		if err != nil {
			return err
		}
	} else {
		dek, rewrap, err = s.unlock(doc)
		if err != nil {
			return err
		}
	}
	defer zero(dek)
	tx := &Tx{doc: doc, dek: dek, now: s.now().UnixMilli()}
	if err := fn(tx); err != nil {
		return err
	}
	if rewrap {
		s.addWrap(doc, dek)
	}
	if tx.dirty || rewrap {
		return s.save(doc, dek)
	}
	return nil
}

func (s *Store) Status() Status {
	s.mu.Lock()
	defer s.mu.Unlock()
	status := Status{State: "empty", Path: s.Path}
	doc, err := s.read()
	if err != nil {
		status.State = stateOf(err)
		return status
	}
	if doc == nil {
		if len(s.signers()) == 0 {
			status.State = "no_identity"
		}
		return status
	}
	status.KeyID = doc.KeyID
	status.Generation = doc.Generation
	status.Count = len(doc.Entries)
	for _, wrap := range doc.Wraps {
		status.Identities = append(status.Identities, wrap.Identity)
	}
	dek, _, err := s.unlock(doc)
	if err != nil {
		status.State = stateOf(err)
		return status
	}
	zero(dek)
	status.State = "ready"
	return status
}

func stateOf(err error) string {
	switch {
	case errors.Is(err, ErrIdentityMismatch):
		return "locked"
	case errors.Is(err, ErrTampered):
		return "tampered"
	case errors.Is(err, ErrNoIdentity):
		return "no_identity"
	case errors.Is(err, ErrFormat):
		return "invalid"
	}
	return "unreadable"
}

func (s *Store) Rotate() (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	doc, err := s.read()
	if err != nil {
		return "", err
	}
	if doc == nil {
		return "", nil
	}
	oldKey, _, err := s.unlock(doc)
	if err != nil {
		return "", err
	}
	defer zero(oldKey)
	newKey := randomBytes(keyBytes)
	defer zero(newKey)
	next := &document{Kind: Kind, V: Version, KeyID: keyIDOf(newKey), Generation: doc.Generation, Entries: map[string]entryRecord{}}
	names := sortedNames(doc.Entries)
	for _, name := range names {
		entry := doc.Entries[name]
		plain, err := openEntry(oldKey, doc.KeyID, name, entry)
		if err != nil {
			return "", err
		}
		next.Entries[name] = sealEntry(newKey, next.KeyID, name, plain, entry.At)
		zero(plain)
	}
	seen := map[string]bool{}
	for _, signer := range s.signers() {
		if seen[signer.PublicKey()] || len(next.Wraps) >= maxWraps {
			continue
		}
		seen[signer.PublicKey()] = true
		next.Wraps = append(next.Wraps, wrapKey(signer, newKey, next.KeyID))
	}
	if err := s.save(next, newKey); err != nil {
		return "", err
	}
	return next.KeyID, nil
}

func SortedKeys[V any](values map[string]V) []string {
	out := make([]string, 0, len(values))
	for key := range values {
		out = append(out, key)
	}
	sort.Strings(out)
	return out
}

func writeAtomic(path string, data []byte) error {
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	_ = os.Chmod(dir, 0o700)
	if info, err := os.Lstat(path); err == nil && !info.Mode().IsRegular() {
		return ErrFormat
	}
	temp, err := os.CreateTemp(dir, ".vault-*.tmp")
	if err != nil {
		return err
	}
	name := temp.Name()
	committed := false
	defer func() {
		if !committed {
			_ = os.Remove(name)
		}
	}()
	if err := temp.Chmod(0o600); err != nil && !isWindows() {
		_ = temp.Close()
		return err
	}
	if _, err := temp.Write(data); err != nil {
		_ = temp.Close()
		return err
	}
	if err := temp.Sync(); err != nil {
		_ = temp.Close()
		return err
	}
	if err := temp.Close(); err != nil {
		return err
	}
	if err := os.Rename(name, path); err != nil {
		return err
	}
	committed = true
	syncDir(dir)
	return nil
}
