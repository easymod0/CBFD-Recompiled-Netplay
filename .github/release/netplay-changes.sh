#!/bin/sh
# The release notes' list of changes since the previous release (netplay-release.yml, this fork only):
#
#   netplay-changes.sh <repo> <previous tag> <commit> <repository url>
#
# prints, as Markdown, the commits in <repo> from <previous tag> to <commit>, newest first, without merges:
# the game's changes, then the release builds' own ("Fork releases: ..."), each linked, and a link to the
# full comparison. A subject is shown as written, but "#<number>" (often upstream's issues, which GitHub
# would link to this fork's), "@<name>" (a mention) and "<" (a tag) don't become links, mentions or tags.
# With no previous tag (the first release), or one that isn't an ancestor of <commit> (the branch was
# rewritten), it says so instead of listing.
set -eu
repo=$1
previous=$2
commit=$3
url=$4
most=60

if [ -z "$previous" ]; then
    echo "## Changes"
    echo
    echo "The first release of this fork."
    exit 0
fi
if ! git -C "$repo" merge-base --is-ancestor "$previous" "$commit" 2>/dev/null; then
    echo "## Changes since $previous"
    echo
    echo "$previous isn't in this release's history, so there's no list: see the [commits]($url/commits/$commit)."
    exit 0
fi

# list <grep option>: one line per commit, with or without the release builds' own. A zero-width space after
# "#" and "@" keeps them plain text. (printf, not echo: sh's echo would read a subject's backslashes.)
list() {
    git -C "$repo" log --no-merges --format='%H %s' "$previous..$commit" | grep "$1" '^[0-9a-f]* Fork releases: ' |
        head -n "$most" | sed 's/#\([0-9]\)/#\&#8203;\1/g; s/@\([A-Za-z0-9]\)/@\&#8203;\1/g; s/</\&lt;/g' |
        while read -r sha subject; do
            printf '%s\n' "- $subject ([${sha%"${sha#???????}"}]($url/commit/$sha))"
        done
}
count() {
    git -C "$repo" log --no-merges --format='%s' "$previous..$commit" | grep -c "$1" '^Fork releases: ' || true
}

game=$(count -v)
builds=$(count -e)
echo "## Changes since $previous"
echo
if [ "$game" -eq 0 ] && [ "$builds" -eq 0 ]; then
    echo "None: the same code as $previous."
else
    if [ "$game" -gt 0 ]; then
        list -v
        [ "$game" -gt "$most" ] && echo "- ...and $((game - most)) more"
    else
        echo "No changes to the game."
    fi
    if [ "$builds" -gt 0 ]; then
        echo
        echo "Release builds:"
        echo
        list -e
        [ "$builds" -gt "$most" ] && echo "- ...and $((builds - most)) more"
    fi
fi
echo
echo "All of it: [$previous...$commit]($url/compare/$previous...$commit)"
