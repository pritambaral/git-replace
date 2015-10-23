## git-replace

Searches for occurrences of `pattern` and replaces them with `replacement` in
commit messages, and (optionally) file names and file contents in the entire git
history of a repo.

## Building
A simple `make` should do. If it doesn't, (and you are sure your make isn't
broken), report an [issue](https://github.com/pritambaral/git-replace/issues/new)

## Usage

```
$ ./git-replace -d /path/to/repo -p 'pattern' -r 'replacement' [-f] [-c]
```

## WARNING
Messing with a git repo's history is not for kiddies. Use only if you truly
understand what consequences rewriting git history can have
