package agents

import (
	"sync"
	"testing"

	"forkmesh.com/desktop-core/internal/qtjson"
)

func TestWriterObserversSeeEveryChange(t *testing.T) {
	writer := NewWriter(NewStore(t.TempDir()))
	var mu sync.Mutex
	seen := []Change{}
	stop := writer.Observe(func(change Change) {
		mu.Lock()
		seen = append(seen, change)
		mu.Unlock()
	})
	record, err := writer.CreateRecord(qtjson.ObjectOf("schemaVersion", 2, "owner", "a", "name", "b", "status", "queued"))
	if err != nil {
		t.Fatal(err)
	}
	id := record.Get("id").Int()
	dir, _ := writer.DirFor("a-b", id)
	writer.AppendLog(dir, id, "x")
	writer.AppendEvent(dir, id, qtjson.ObjectOf("type", "x"))
	record.Set("status", qtjson.NewString("running"))
	if _, err := writer.SaveRecord(record); err != nil {
		t.Fatal(err)
	}
	if _, err := writer.SaveRecord(record); err != nil {
		t.Fatal(err)
	}
	if ok, err := writer.RecycleRecord(record); !ok || err != nil {
		t.Fatal(ok, err)
	}
	if ok, err := writer.RestoreRecycled(id); !ok || err != nil {
		t.Fatal(ok, err)
	}
	if ok, err := writer.RecycleRecord(record); !ok || err != nil {
		t.Fatal(ok, err)
	}
	if ok, err := writer.PurgeRecycled(id); !ok || err != nil {
		t.Fatal(ok, err)
	}
	writer.Flush()
	stop()
	writer.AppendLog(dir, id, "after")
	want := []Change{{ChangeRecord, id}, {ChangeLog, id}, {ChangeEvent, id}, {ChangeRecord, id}, {ChangeRemoved, id},
		{ChangeRecord, id}, {ChangeRestored, id}, {ChangeRemoved, id}, {ChangeBin, id}}
	mu.Lock()
	defer mu.Unlock()
	if len(seen) != len(want) {
		t.Fatalf("seen %v want %v", seen, want)
	}
	for i := range want {
		if seen[i] != want[i] {
			t.Fatalf("seen %v want %v", seen, want)
		}
	}
}
