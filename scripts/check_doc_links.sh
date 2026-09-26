#!/usr/bin/env bash
# Fail the docs build if a branch leaves the documentation set with more
# Sphinx warnings than it found, in any .rst file that the branch touched.
#
# Sphinx has to build the whole documentation set to resolve cross references,
# but legacy pages (the "Old Wiki" toctree and pages not yet folded into the
# new structure) still carry warnings. Demanding zero of them would mean every
# documentation change had to clean up the whole backlog first, which in
# practice only gets the gate disabled. Compare instead: build the merge base
# and the branch, and fail when a touched file gains a warning the page did
# not already have. Warnings a page brought with it may stay, and removing
# them is always welcome; adding one does not pass.
#
# The comparison is per warning, not per total. Counting warnings per page
# would let a change that introduces a new warning pass as long as it removed
# more than it added, which is the opposite of what this gate is for. Line
# numbers are left out of the comparison because any edit shifts them.
#
# Usage: bash scripts/check_doc_links.sh
#   DOCS_BASE   base ref or SHA to diff against; for a pull request this is
#               the base branch SHA, for a push it is the "before" SHA.
#   DOCS_EVENT  path to the GitHub event payload, used to recover the base
#               when DOCS_BASE is empty.

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="$(mktemp -d)"
base_tree="$build_root/base"
trap 'git -C "$root" worktree remove --force "$base_tree" >/dev/null 2>&1; rm -rf "$build_root"' EXIT

cd "$root"

# Resolve the base to diff against. The only refs that describe the intended
# merge point are the pull request's base SHA and the push event's "before"
# SHA, so take one of those and refuse to guess. Falling back to a moving
# branch like origin/trunk would quietly judge the change against a different
# set of files whenever the two disagree, which is worse than failing.
base="${DOCS_BASE:-}"
if [ -z "$base" ] && [ -n "${DOCS_EVENT:-}" ] && [ -f "$DOCS_EVENT" ]; then
    base="$(jq -r '.pull_request.base.sha // .before // empty' "$DOCS_EVENT")"
fi
if [ -z "$base" ]; then
    echo "No base to diff against: set DOCS_BASE, or point DOCS_EVENT at the event payload." >&2
    exit 1
fi

# A three-dot diff is really a merge-base diff, but git still needs the base
# to be a commit it can see. Resolve the merge point explicitly so a base
# that has moved (or a force-push whose "before" is gone) is reported as
# such instead of being papered over.
if ! mb="$(git merge-base "$base" HEAD 2>/dev/null)" || [ -z "$mb" ]; then
    echo "'$base' is not usable as a diff base in this checkout." >&2
    echo "Fetch it first, or set DOCS_BASE to a ref that is present." >&2
    exit 1
fi

# NUL-separated so that paths containing spaces or quotes survive, and with
# quoting disabled so the list is not mangled by git's C-style escaping.
changed=()
while IFS= read -r -d '' file; do
    changed+=("$file")
done < <(git -c core.quotePath=false diff --name-only -z "$mb..HEAD" -- '*.rst')

# Build unconditionally. Sphinx has to see the whole tree to resolve
# references, and a branch that touches only docs/conf.py, the requirements
# or an image can still break every page. Returning early when no .rst
# changed reported those as a pass, which is exactly the case the gate
# exists to catch.
head_log="$build_root/head.log"
sphinx-build -q --keep-going -b html docs "$build_root/html" 2> "$head_log"
code=$?
if [ $code -ne 0 ]; then
    echo "sphinx-build failed with exit code $code." >&2
    cat "$head_log" >&2
    exit $code
fi

if [ "${#changed[@]}" -eq 0 ]; then
    echo "No .rst files changed since $mb ($base); the documentation set still builds."
    exit 0
fi

# Build the merge base too, so every warning the branch reports can be checked
# against the warnings the page already had. The check needs the base tree
# rather than the base blob because Sphinx resolves references across the whole
# documentation set, so a page's warnings depend on the other pages as well.
if ! git worktree add --quiet --detach "$base_tree" "$mb" 2>/dev/null; then
    echo "Could not check out the merge base $mb to compare warnings." >&2
    exit 1
fi
base_log="$build_root/base.log"
sphinx-build -q --keep-going -b html "$base_tree/docs" "$build_root/base_html" 2> "$base_log"
code=$?
if [ $code -ne 0 ]; then
    echo "The merge base $mb does not build cleanly (sphinx-build exit code $code)," >&2
    echo "so it cannot serve as a baseline. Fix trunk first." >&2
    exit $code
fi

# Reduce each warning to "<page><TAB><message>", with the page normalised to
# a path relative to the docs source directory so it can be compared against
# git's list, and the line number dropped so an edit elsewhere on the page
# does not read as a new warning. Sphinx reports absolute paths for documents
# and source-relative ones for included files such as CHANGELOG.md, and it
# omits the line number for toctree warnings; comparing the raw text instead
# let a touched page inherit the warnings of any page whose path merely
# contained its own.
blame() {
    awk -v root="$root/" -v btree="$base_tree/" '
        index($0, ": WARNING: ") {
            msg = $0
            path = $0
            sub(/:[0-9]+: WARNING:.*$/, "", path)
            sub(/: WARNING:.*$/, "", path)
            sub("^" root "docs/", "", path)
            sub("^" root, "", path)
            sub("^" btree "docs/", "", path)
            sub("^" btree, "", path)
            sub("^docs/", "", path)
            sub(/^.*: WARNING: /, "", msg)
            print path "\t" msg
        }
    ' "$1"
}

blame "$base_log" > "$build_root/base.keys"
blame "$head_log" > "$build_root/head.keys"

# A warning is new when the branch reports it more often than the base did.
# Comparing multiplicities rather than membership keeps the check stable for
# pages that repeat the same warning on several lines.
awk -F'\t' '
    FNR == NR { base[$1 SUBSEP $2]++; next }
               { head[$1 SUBSEP $2]++ }
    END {
        for (key in head) {
            if (head[key] > (key in base ? base[key] : 0)) {
                split(key, part, SUBSEP)
                print part[1] "\t" part[2]
            }
        }
    }
' "$build_root/base.keys" "$build_root/head.keys" | LC_ALL=C sort -u > "$build_root/new.keys"

hits=0
for file in "${changed[@]}"; do
    rel="${file#docs/}"
    [ -n "$rel" ] || continue
    # Compare the whole first field, never a substring of it: a bare
    # index.rst is a different page from user/faq/index.rst, and testing
    # for the shorter name inside the longer one blames the wrong page.
    if ! awk -F'\t' -v want="$rel" '$1 == want { found = 1 } END { exit !found }' "$build_root/new.keys"; then
        continue
    fi
    printf '::error file=%s::Sphinx reported a warning that this page did not have before\n' "$file"
    awk -F'\t' -v want="$rel" '$1 == want { print "  " $2 }' "$build_root/new.keys"
    hits=1
done

if [ "$hits" -eq 0 ]; then
    echo "No page this branch touched gained a Sphinx warning since $mb."
    exit 0
fi

exit 1