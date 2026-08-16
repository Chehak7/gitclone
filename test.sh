#!/usr/bin/env bash

set -u

project_dir=$(cd "$(dirname "$0")" && pwd)
mygit="$project_dir/mygit"
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/cppgit-test.XXXXXX") || {
    echo "FAIL: could not create temporary directory" >&2
    exit 1
}
trap 'rm -rf "$test_dir"' EXIT

fail() {
    echo "FAIL: $1" >&2
    if [[ -n ${output:-} ]]; then
        echo "$output" >&2
    fi
    exit 1
}

run() {
    output=$("$mygit" "$@" 2>&1) || fail "mygit $* exited non-zero"
}

run_fails() {
    if output=$("$mygit" "$@" 2>&1); then
        fail "mygit $* unexpectedly succeeded"
    fi
}

[[ -x "$mygit" ]] || fail "binary not found or not executable: $mygit"
cd "$test_dir" || fail "could not enter temporary directory"

run init
[[ -d .git/objects ]] || fail "init did not create .git/objects"
[[ -f .git/HEAD ]] || fail "init did not create .git/HEAD"
grep -qx 'ref: refs/heads/master' .git/HEAD ||
    fail "init did not point HEAD at master"

printf 'alpha\n' > alpha.txt || fail "could not create file fixture"
run add alpha.txt
[[ -s .git/index ]] || fail "adding a file did not update the index"

mkdir docs || fail "could not create directory fixture"
printf 'beta\n' > docs/beta.txt || fail "could not create directory fixture file"
printf 'gamma\n' > docs/gamma.txt || fail "could not create directory fixture file"
run add docs
[[ $output == *"Added 2 files from directory docs"* ]] ||
    fail "adding a directory did not report both files"

commit_message='full CLI test commit'
run commit -m "$commit_message"
[[ $output == *"Created commit "*" on branch master"* ]] ||
    fail "commit did not report creation on master"
[[ -s .git/refs/heads/master ]] || fail "commit did not create the master ref"

run log
[[ $output == *"$commit_message"* ]] || fail "log omitted the commit message"
[[ $output == *"Author: "* ]] || fail "log omitted the author"

run status
[[ $output == *"On branch master"* ]] || fail "status reported the wrong branch"
[[ $output == *"nothing to commit, working tree clean"* ]] ||
    fail "status did not report a clean working tree"

run branch
[[ $output == *"* master"* ]] || fail "branch did not mark master as current"

run branch feature
[[ -s .git/refs/heads/feature ]] || fail "branch creation did not create feature ref"
cmp -s .git/refs/heads/master .git/refs/heads/feature ||
    fail "feature did not start at the current commit"

run checkout -b topic
grep -qx 'ref: refs/heads/topic' .git/HEAD ||
    fail "checkout -b did not switch HEAD to topic"
[[ -s .git/refs/heads/topic ]] || fail "checkout -b did not create topic ref"

run checkout feature
grep -qx 'ref: refs/heads/feature' .git/HEAD ||
    fail "checkout existing branch did not switch HEAD to feature"

run branch topic -d
[[ ! -e .git/refs/heads/topic ]] || fail "branch deletion left the topic ref"

run branch
[[ $output == *"* feature"* ]] || fail "final branch list did not mark feature current"
[[ $output != *"topic"* ]] || fail "deleted topic branch still appeared in branch list"

# Clean three-way merge: each side changes a different file.
mkdir "$test_dir/clean-merge" || fail "could not create clean merge fixture"
cd "$test_dir/clean-merge" || fail "could not enter clean merge fixture"
run init
printf 'current base\n' > current.txt
printf 'target base\n' > target.txt
run add .
run commit -m 'merge base'
run branch target
printf 'current changed\n' > current.txt
run add .
run commit -m 'current-side change'
current_parent=$(tr -d '\n' < .git/refs/heads/master)
run checkout target
printf 'target changed\n' > target.txt
run add .
run commit -m 'target-side change'
target_parent=$(tr -d '\n' < .git/refs/heads/target)
run checkout master
run merge target
[[ $output == *"Merge made commit "* ]] || fail "clean merge did not create a merge commit"
grep -qx 'current changed' current.txt || fail "clean merge lost the current-side change"
grep -qx 'target changed' target.txt || fail "clean merge lost the target-side change"
merge_hash=$(tr -d '\n' < .git/refs/heads/master)
[[ $merge_hash != "$current_parent" && $merge_hash != "$target_parent" ]] ||
    fail "clean merge did not advance to a new commit"
merge_object=$(git cat-file -p "$merge_hash" 2>&1) || fail "could not inspect merge commit"
[[ $(grep -c '^parent ' <<< "$merge_object") -eq 2 ]] ||
    fail "clean merge commit does not have two parents"
[[ $merge_object == *"parent $current_parent"* ]] ||
    fail "merge commit omitted the current parent"
[[ $merge_object == *"parent $target_parent"* ]] ||
    fail "merge commit omitted the target parent"
run status
[[ $output == *"nothing to commit, working tree clean"* ]] ||
    fail "working tree was not clean after clean merge"

# Fast-forward merge and dirty-tree refusal.
mkdir "$test_dir/fast-forward" || fail "could not create fast-forward fixture"
cd "$test_dir/fast-forward" || fail "could not enter fast-forward fixture"
run init
printf 'base\n' > ff.txt
run add ff.txt
run commit -m 'fast-forward base'
run checkout -b ahead
printf 'ahead\n' > ff.txt
run add ff.txt
run commit -m 'fast-forward target'
ahead_hash=$(tr -d '\n' < .git/refs/heads/ahead)
run checkout master
printf 'dirty\n' > ff.txt
run_fails merge ahead
[[ $output == *"would be overwritten by merge"* ]] ||
    fail "merge did not refuse a dirty working tree"
[[ $(tr -d '\n' < .git/refs/heads/master) != "$ahead_hash" ]] ||
    fail "dirty-tree refusal moved the current branch"
printf 'base\n' > ff.txt
run merge ahead
[[ $output == *"Fast-forward"* ]] || fail "merge did not report fast-forward"
[[ $(tr -d '\n' < .git/refs/heads/master) == "$ahead_hash" ]] ||
    fail "fast-forward did not move the current branch to the target"
grep -qx 'ahead' ff.txt || fail "fast-forward did not update the working file"

# Conflicting three-way merge: both sides change the same content differently.
mkdir "$test_dir/conflict-merge" || fail "could not create conflict fixture"
cd "$test_dir/conflict-merge" || fail "could not enter conflict fixture"
run init
printf 'base line\n' > shared.txt
run add .
run commit -m 'conflict base'
run branch other
printf 'current line\n' > shared.txt
run add .
run commit -m 'current conflict change'
pre_merge_hash=$(tr -d '\n' < .git/refs/heads/master)
run checkout other
printf 'target line\n' > shared.txt
run add .
run commit -m 'target conflict change'
run checkout master
run_fails merge other
[[ $output == *"CONFLICT"* ]] || fail "conflicting merge did not report a conflict"
[[ $output == *"Automatic merge failed"* ]] ||
    fail "conflicting merge omitted the automatic-merge failure message"
grep -qx '<<<<<<< HEAD' shared.txt || fail "conflict file omitted the HEAD marker"
grep -qx '=======' shared.txt || fail "conflict file omitted the separator marker"
grep -qx '>>>>>>> other' shared.txt || fail "conflict file omitted the target marker"
grep -qx 'current line' shared.txt || fail "conflict file omitted current content"
grep -qx 'target line' shared.txt || fail "conflict file omitted target content"
post_merge_hash=$(tr -d '\n' < .git/refs/heads/master)
[[ $post_merge_hash == "$pre_merge_hash" ]] ||
    fail "conflicting merge created or advanced a commit"

echo "PASS: full CLI test completed in $test_dir"
