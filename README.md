# mygit

`mygit` is a small Git implementation written in a single C++17 source file. It stores Git-style blobs, trees, and commits as compressed objects, tracks files through an index, supports branches and checkout, and performs basic three-way merges.

This is an educational project rather than a replacement for Git. It is useful for exploring how content-addressed objects, commits, refs, working trees, and merge ancestry fit together.

## Features

- Git-style SHA-1 object IDs and zlib-compressed loose objects
- File and recursive directory staging
- Commits, history, and working-tree status
- Branch creation, listing, deletion, and checkout
- Three-way merges with two-parent merge commits
- Fast-forward merge detection
- Standard conflict markers when both branches change the same file
- Protection against checkout or merge overwriting local changes
- Automated CLI tests using disposable repositories

## Requirements

- macOS with CommonCrypto
- A C++17 compiler such as `g++` or `clang++`
- zlib
- `make`
- Bash and real Git for the automated test script

## Build

```bash
git clone https://github.com/Chehak7/gitclone.git
cd gitclone
make
```

This produces a `mygit` executable in the project directory. To rebuild from scratch:

```bash
make clean && make
```

## Commands

| Command | Description |
| --- | --- |
| `./mygit init` | Initialize a repository in the current directory |
| `./mygit add <path> [<path> ...]` | Add files or directories to the staging area |
| `./mygit commit -m "<message>"` | Create a commit from the staged files |
| `./mygit checkout <branch>` | Switch to an existing branch |
| `./mygit checkout -b <branch>` | Create and switch to a new branch |
| `./mygit merge <branch>` | Merge a branch into the current branch |
| `./mygit branch` | List branches |
| `./mygit branch <name>` | Create a branch at the current commit |
| `./mygit branch <name> -d` | Delete a branch |
| `./mygit log` | Show commit history |
| `./mygit log -n <count>` | Limit the number of displayed commits |
| `./mygit status` | Show staged, modified, deleted, and untracked files |

## Quick start

Run `mygit` inside a separate directory so its `.git` data does not overlap with this project's real Git repository:

```bash
mkdir /tmp/mygit-demo
cd /tmp/mygit-demo

/path/to/gitclone/mygit init
printf 'hello\n' > hello.txt
/path/to/gitclone/mygit add hello.txt
/path/to/gitclone/mygit commit -m "add greeting"
/path/to/gitclone/mygit log
/path/to/gitclone/mygit status
```

## Branches and merges

```bash
./mygit checkout -b feature

printf 'feature work\n' > feature.txt
./mygit add .
./mygit commit -m "add feature work"

./mygit checkout master
./mygit merge feature
```

If both branches change the same file differently, `mygit` leaves the branch pointer unchanged and writes conflict markers into the working file:

```text
<<<<<<< HEAD
content from the current branch
=======
content from the merged branch
>>>>>>> feature
```

Resolve the file, add it again, and commit the result manually.

## CLI demonstration

The screenshot below comes from a successful run of every supported command, including a clean three-way merge:

<img width="1200" height="1250" alt="CLI_demo" src="https://github.com/user-attachments/assets/5614982c-081f-4ea6-9bb9-a30a986aa362" />


## Tests

The test script builds histories inside temporary directories and checks the complete CLI surface, clean merges, fast-forwards, dirty-tree refusal, and conflicts:

```bash
make clean && make
./test.sh
```

A successful run ends with:

```text
PASS: full CLI test completed
```

Temporary repositories are removed automatically when the test finishes.

## Project structure

```text
.
├── main.cpp          # CLI and repository implementation
├── Makefile          # C++17 build
├── test.sh           # End-to-end CLI tests
└── docs/
    └── cli-demo.png  # Output from a successful CLI run
```

## Notes

- Repository data is stored under `.git`, including `HEAD`, the index, object files, and branch refs.
- The default branch is `master`.
- Commits use the built-in identity `CppGit User <user@cppgit.com>` unless another author is supplied.
- The generated `mygit` binary is ignored by the project's real Git repository.

## License

No license has been specified yet.
