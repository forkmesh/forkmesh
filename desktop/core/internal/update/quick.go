package update

import (
	"os"
	"path/filepath"
)

const defaultRepoURL = "https://github.com/forkmesh/forkmesh.git"

func (j *job) quickPullArguments(clientDir string) []string {
	if upstream, ok := j.quiet(clientDir, "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"); ok && upstream != "" {
		return []string{"pull", "--ff-only"}
	}
	if branch, ok := j.quiet(clientDir, "branch", "--show-current"); ok && branch != "" {
		return []string{"pull", "--ff-only", "origin", branch}
	}
	return []string{"pull", "--ff-only", "origin", "HEAD"}
}

func (j *job) quickPull() outcome {
	clientDir := j.req.ClientDir
	if _, err := os.Stat(filepath.Join(clientDir, "CMakeLists.txt")); err == nil {
		j.out.status("Pulling the latest version...", false)
		if result := j.step("git", j.quickPullArguments(clientDir), clientDir, nil); !result.ok {
			return j.failWith(result)
		}
		return outcomeInstalled
	}
	repoDir := filepath.Dir(filepath.Clean(clientDir))
	parent := filepath.Dir(repoDir)
	_ = os.MkdirAll(parent, 0o777)
	j.out.status("Downloading the latest version...", false)
	url := j.req.RepoURL
	if url == "" {
		url = defaultRepoURL
	}
	if result := j.step("git", []string{"clone", "--depth", "1", url, repoDir}, parent, nil); !result.ok {
		return j.failWith(result)
	}
	return outcomeInstalled
}
