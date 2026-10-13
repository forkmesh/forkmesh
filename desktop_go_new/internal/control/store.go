package control

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
)

type Device struct {
	Key              string
	Name             string
	User             string
	Platform         string
	PairedAtMs       int64
	LastSeenMs       int64
	Approval         string
	AccountCheckedMs int64
}

func (d Device) record() map[string]any {
	object := map[string]any{
		"key":        d.Key,
		"name":       d.Name,
		"user":       d.User,
		"platform":   d.Platform,
		"pairedAtMs": d.PairedAtMs,
		"lastSeenMs": d.LastSeenMs,
	}
	if d.Approval != "" {
		object["approval"] = d.Approval
	}
	if d.AccountCheckedMs > 0 {
		object["accountCheckedMs"] = d.AccountCheckedMs
	}
	return object
}

func deviceFromObject(object map[string]any) Device {
	return Device{
		Key:              jsonString(object["key"]),
		Name:             boundedText(object, "name", maxNameChars),
		User:             boundedText(object, "user", maxNameChars),
		Platform:         boundedText(object, "platform", 16),
		PairedAtMs:       int64(jsonDouble(object["pairedAtMs"], 0)),
		LastSeenMs:       int64(jsonDouble(object["lastSeenMs"], 0)),
		Approval:         boundedText(object, "approval", 16),
		AccountCheckedMs: int64(jsonDouble(object["accountCheckedMs"], 0)),
	}
}

func sortedDevices(devices map[string]Device) []Device {
	out := make([]Device, 0, len(devices))
	for _, device := range devices {
		out = append(out, device)
	}
	sort.SliceStable(out, func(i, j int) bool {
		if out[i].PairedAtMs != out[j].PairedAtMs {
			return out[i].PairedAtMs < out[j].PairedAtMs
		}
		return out[i].Key < out[j].Key
	})
	return out
}

func LoadDevices(path string) map[string]Device {
	devices := map[string]Device{}
	if path == "" {
		return devices
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return devices
	}
	list, _ := parseObject(raw)["devices"].([]any)
	for _, value := range list {
		device := deviceFromObject(jsonObject(value))
		if FromBase64Url(device.Key, keyBytes) == nil {
			continue
		}
		devices[device.Key] = device
	}
	return devices
}

func encodeStore(devices map[string]Device) []byte {
	list := make([]any, 0, len(devices))
	for _, device := range sortedDevices(devices) {
		list = append(list, device.record())
	}
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	encoder.SetIndent("", "    ")
	_ = encoder.Encode(map[string]any{"devices": list})
	return buffer.Bytes()
}

func SaveDevices(path string, devices map[string]Device) error {
	if path == "" {
		return nil
	}
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	temp, err := os.CreateTemp(dir, filepath.Base(path)+".*.tmp")
	if err != nil {
		return err
	}
	name := temp.Name()
	_, werr := temp.Write(encodeStore(devices))
	serr := temp.Sync()
	cerr := temp.Close()
	if werr != nil || serr != nil || cerr != nil {
		_ = os.Remove(name)
		if werr != nil {
			return werr
		}
		if serr != nil {
			return serr
		}
		return cerr
	}
	if err := os.Rename(name, path); err != nil {
		_ = os.Remove(name)
		return err
	}
	return nil
}
