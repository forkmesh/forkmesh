package mirrornode

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"math"
	"os"
	"path/filepath"
)

// Sealed operator files — node backups and identity exports — carry the
// node's private key and connector token, so they are never written in the
// clear. The format is a STREAM construction over AES-256-GCM: a fixed header
// (magic, PBKDF2-SHA256 salt and iteration count, nonce prefix) authenticated
// as associated data, followed by 1 MiB chunks whose nonce binds the chunk
// counter and a final-chunk flag. A wrong passphrase and any reordered,
// altered, dropped, or truncated chunk all fail to open.
const (
	sealedMagic            = "FMSEAL01"
	sealedChunkBytes       = 1 << 20
	sealedMinKDFIterations = 100000
	sealedMaxKDFIterations = 10000000
	sealedSaltBytes        = 16
	sealedNoncePrefixBytes = 7
	minimumPassphraseBytes = 12
	maximumPassphraseBytes = 4096
)

// sealedKDFIterations is a variable only so tests can use the minimum.
var sealedKDFIterations = 600000

var errSealedOpen = errors.New("the passphrase is wrong or the sealed file is damaged")

// ReadPassphraseFile reads an operator passphrase from an owner-only file. A
// passphrase is never accepted in argv or the environment, where it would be
// visible to other local users and to process listings.
func ReadPassphraseFile(path string) ([]byte, error) {
	if !filepath.IsAbs(path) {
		return nil, errors.New("--passphrase-file must be absolute")
	}
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() > maximumPassphraseBytes+2 {
		return nil, errors.New("passphrase file must be a small regular file")
	}
	if info.Mode().Perm()&0077 != 0 {
		return nil, errors.New("passphrase file must be owner-only")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	raw = bytes.TrimRight(raw, "\r\n")
	if len(raw) < minimumPassphraseBytes {
		return nil, fmt.Errorf("passphrase must be at least %d bytes", minimumPassphraseBytes)
	}
	return raw, nil
}

func sealedCipher(passphrase, salt []byte, iterations int) (cipher.AEAD, error) {
	key, err := pbkdf2.Key(sha256.New, string(passphrase), salt, iterations, 32)
	if err != nil {
		return nil, err
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	return cipher.NewGCM(block)
}

func sealedNonce(prefix []byte, counter uint64, final bool) []byte {
	nonce := make([]byte, 0, sealedNoncePrefixBytes+5)
	nonce = append(nonce, prefix...)
	nonce = binary.BigEndian.AppendUint32(nonce, uint32(counter))
	if final {
		return append(nonce, 1)
	}
	return append(nonce, 0)
}

type sealWriter struct {
	out     io.Writer
	aead    cipher.AEAD
	header  []byte
	prefix  []byte
	counter uint64
	buffer  []byte
	sealed  []byte
	closed  bool
}

func newSealWriter(out io.Writer, passphrase []byte) (*sealWriter, error) {
	salt := make([]byte, sealedSaltBytes)
	prefix := make([]byte, sealedNoncePrefixBytes)
	if _, err := rand.Read(salt); err != nil {
		return nil, err
	}
	if _, err := rand.Read(prefix); err != nil {
		return nil, err
	}
	header := append([]byte(sealedMagic), salt...)
	header = binary.BigEndian.AppendUint32(header, uint32(sealedKDFIterations))
	header = append(header, prefix...)
	aead, err := sealedCipher(passphrase, salt, sealedKDFIterations)
	if err != nil {
		return nil, err
	}
	if _, err := out.Write(header); err != nil {
		return nil, err
	}
	return &sealWriter{
		out: out, aead: aead, header: header, prefix: prefix,
		buffer: make([]byte, 0, sealedChunkBytes),
	}, nil
}

func (w *sealWriter) Write(data []byte) (int, error) {
	if w.closed {
		return 0, errors.New("write to a closed sealed file")
	}
	written := 0
	for len(data) > 0 {
		// A full buffer is sealed as a non-final chunk only once more data
		// arrives, so Close always has a final chunk (possibly empty) to seal.
		if len(w.buffer) == sealedChunkBytes {
			if err := w.flush(false); err != nil {
				return written, err
			}
		}
		take := min(sealedChunkBytes-len(w.buffer), len(data))
		w.buffer = append(w.buffer, data[:take]...)
		data = data[take:]
		written += take
	}
	return written, nil
}

func (w *sealWriter) flush(final bool) error {
	if w.counter > math.MaxUint32 {
		return errors.New("sealed file is too large")
	}
	w.sealed = w.aead.Seal(w.sealed[:0], sealedNonce(w.prefix, w.counter, final), w.buffer, w.header)
	w.counter++
	w.buffer = w.buffer[:0]
	_, err := w.out.Write(w.sealed)
	return err
}

func (w *sealWriter) Close() error {
	if w.closed {
		return nil
	}
	w.closed = true
	return w.flush(true)
}

type openReader struct {
	in      io.Reader
	aead    cipher.AEAD
	header  []byte
	prefix  []byte
	counter uint64
	chunk   []byte
	plain   []byte
	pending []byte
	done    bool
}

func newOpenReader(in io.Reader, passphrase []byte) (*openReader, error) {
	header := make([]byte, len(sealedMagic)+sealedSaltBytes+4+sealedNoncePrefixBytes)
	if _, err := io.ReadFull(in, header); err != nil || string(header[:len(sealedMagic)]) != sealedMagic {
		return nil, errors.New("not a sealed ForkMesh operator file")
	}
	offset := len(sealedMagic)
	salt := header[offset : offset+sealedSaltBytes]
	iterations := binary.BigEndian.Uint32(header[offset+sealedSaltBytes:])
	if iterations < sealedMinKDFIterations || iterations > sealedMaxKDFIterations {
		return nil, errors.New("sealed file key-derivation parameters are unsupported")
	}
	aead, err := sealedCipher(passphrase, salt, int(iterations))
	if err != nil {
		return nil, err
	}
	return &openReader{
		in: in, aead: aead, header: header,
		prefix: header[offset+sealedSaltBytes+4:],
		chunk:  make([]byte, sealedChunkBytes+aead.Overhead()),
		plain:  make([]byte, 0, sealedChunkBytes),
	}, nil
}

func (r *openReader) Read(data []byte) (int, error) {
	for len(r.pending) == 0 {
		if r.done {
			return 0, io.EOF
		}
		if err := r.next(); err != nil {
			return 0, err
		}
	}
	n := copy(data, r.pending)
	r.pending = r.pending[n:]
	return n, nil
}

func (r *openReader) next() error {
	if r.counter > math.MaxUint32 {
		return errSealedOpen
	}
	n, err := io.ReadFull(r.in, r.chunk)
	switch {
	case err == io.EOF:
		return errors.New("sealed file is truncated")
	case err == io.ErrUnexpectedEOF:
		return r.open(r.chunk[:n], true)
	case err != nil:
		return err
	}
	if r.open(r.chunk, false) == nil {
		return nil
	}
	if err := r.open(r.chunk, true); err != nil {
		return err
	}
	var extra [1]byte
	if n, _ := io.ReadFull(r.in, extra[:]); n != 0 {
		return errors.New("sealed file has trailing data")
	}
	return nil
}

func (r *openReader) open(sealed []byte, final bool) error {
	plain, err := r.aead.Open(r.plain[:0], sealedNonce(r.prefix, r.counter, final), sealed, r.header)
	if err != nil {
		return errSealedOpen
	}
	r.pending = plain
	r.counter++
	r.done = final
	return nil
}

type countingHashWriter struct {
	out   io.Writer
	hash  io.Writer
	count int64
}

func (w *countingHashWriter) Write(data []byte) (int, error) {
	n, err := w.out.Write(data)
	w.hash.Write(data[:n])
	w.count += int64(n)
	return n, err
}

// writeSealedFile seals everything produce writes into a new owner-only file.
// The file is assembled under a temporary name, fsynced, and then linked into
// place, so an existing file is never replaced and a failed run leaves nothing
// behind. It returns the sealed size and SHA-256.
func writeSealedFile(path string, passphrase []byte, produce func(io.Writer) error) (int64, string, error) {
	if !filepath.IsAbs(path) {
		return 0, "", errors.New("output path must be absolute")
	}
	if _, err := os.Lstat(path); err == nil {
		return 0, "", fmt.Errorf("refusing to overwrite existing %s", path)
	}
	temporary, err := os.CreateTemp(filepath.Dir(path), ".forkmesh-sealed-*")
	if err != nil {
		return 0, "", err
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	defer temporary.Close()
	if err := temporary.Chmod(0600); err != nil {
		return 0, "", err
	}
	digest := sha256.New()
	counter := &countingHashWriter{out: temporary, hash: digest}
	sealer, err := newSealWriter(counter, passphrase)
	if err != nil {
		return 0, "", err
	}
	if err := produce(sealer); err != nil {
		return 0, "", err
	}
	if err := sealer.Close(); err != nil {
		return 0, "", err
	}
	if err := temporary.Sync(); err != nil {
		return 0, "", err
	}
	if err := temporary.Close(); err != nil {
		return 0, "", err
	}
	if err := os.Link(temporaryPath, path); err != nil {
		if errors.Is(err, fs.ErrExist) {
			return 0, "", fmt.Errorf("refusing to overwrite existing %s", path)
		}
		// Some backup targets (FAT, a few network filesystems) have no hard
		// links; fall back to a rename after re-checking the destination.
		if _, statErr := os.Lstat(path); statErr == nil {
			return 0, "", fmt.Errorf("refusing to overwrite existing %s", path)
		}
		if err := os.Rename(temporaryPath, path); err != nil {
			return 0, "", err
		}
	}
	return counter.count, hex.EncodeToString(digest.Sum(nil)), nil
}
