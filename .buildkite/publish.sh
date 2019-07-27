#!/bin/bash

set -euo pipefail

echo "--- setup"
apt-get update
apt-get install -yy curl jq rubygems

git config --global user.email "sorbet+bot@stripe.com"
git config --global user.name "Sorbet build farm"

git_commit_count=$(git rev-list --count HEAD)
prefix="0.4"
release_version="0.4.${git_commit_count}"
long_release_version="${release_version}.$(git log --format=%cd-%h --date=format:%Y%m%d%H%M%S -1)"

dryrun="1"
changes=""
if [ "schedule" == "${BUILDKITE_SOURCE}" ]; then
  if [ "$BUILDKITE_BRANCH" == 'master' ]; then
    # publish on scheduled builds of master
    dryrun=""
    last_released_version="$(curl -s https://rubygems.org/api/v1/gems/sorbet.json | jq '.["version"]' | rev | cut -d '.' -f 1 | cut -d "\"" -f 2 | rev)"
    new_commits=$((git_commit_count - last_released_version)) # how many new commits are there in master
    if [ "0" == "$new_commits" ]; then
      # no new commits, don't actually do stuff
      dryrun="1"
    fi
    changes="$(git log -n ${new_commits} --pretty=format:" - %h %s by %an")"
  fi
fi


echo "--- Dowloading artifacts"
rm -rf release
rm -rf _out_
buildkite-agent artifact download "_out_/**/*" .

echo "--- releasing sorbet.run"

rm -rf sorbet.run
git clone git@github.com:sorbet/sorbet.run.git
tar -xvf ./_out_/webasm/sorbet-wasm.tar ./sorbet-wasm.wasm ./sorbet-wasm.js
mv sorbet-wasm.wasm sorbet.run/docs
mv sorbet-wasm.js sorbet.run/docs
pushd sorbet.run/docs
git add sorbet-wasm.wasm sorbet-wasm.js
dirty=
git diff-index --quiet HEAD -- || dirty=1
if [ "$dirty" != "" ]; then
  echo "$BUILDKITE_COMMIT" > sha.html
  git add sha.html
  git commit -m "Updated site - $(date -u +%Y-%m-%dT%H:%M:%S%z)"
  if [ "$dryrun" = "" ]; then
      git push
  fi
else
  echo "Nothing to update"
fi
popd
rm -rf sorbet.run

echo "--- releasing sorbet.org"
git fetch origin gh-pages
current_rev=$(git rev-parse HEAD)
git checkout gh-pages
# Remove all tracked files, but leave untracked files (like _out_) untouched
git rm -rf '*'
tar -xjf _out_/website/website.tar.bz2 .
git add .
git reset HEAD _out_
dirty=
git diff-index --quiet HEAD -- || dirty=1
if [ "$dirty" != "" ]; then
  echo "$BUILDKITE_COMMIT" > sha.html
  git add sha.html
  git commit -m "Updated site - $(date -u +%Y-%m-%dT%H:%M:%S%z)"
  if [ "$dryrun" = "" ]; then
      git push origin gh-pages

      # For some reason, GitHub Pages won't automatically build for us on push
      # We have a ticket open with GitHub to investigate why.
      # For now, we trigger a build manually.
      curl \
        -X POST \
        --netrc \
        -H "Accept: application/vnd.github.mister-fantastic-preview+json" \
        "https://api.github.com/repos/sorbet/sorbet/pages/builds"
  fi
  echo "pushed an update"
else
  echo "nothing to update"
fi
git checkout -f "$current_rev"

echo "--- publishing gems to RubyGems.org"

mkdir -p "$HOME/.gem"
printf -- $'---\n:rubygems_api_key: %s\n' "$RUBY_GEMS_API_KEY" > "$HOME/.gem/credentials"
chmod 600 "$HOME/.gem/credentials"

if [ "$dryrun" = "" ]; then
  # push the sorbet-static gems first, in case they fail. We don't want to end
  # up in a weird state where 'sorbet' requires a pinned version of
  # sorbet-static, but the sorbet-static gem push failed.
  #
  # (By failure here, we mean that RubyGems.org 502'd for some reason.)
  for gem_archive in "_out_/gems/sorbet-static-$release_version"-*.gem; do
    gem push "$gem_archive"
  done

  gem push "_out_/gems/sorbet-runtime-$release_version.gem"
  gem push "_out_/gems/sorbet-$release_version.gem"
fi

echo "--- making a github release"
echo releasing "${long_release_version}"
git tag -f "${long_release_version}"
if [ "$dryrun" = "" ]; then
    git push origin "${long_release_version}"
fi

mkdir release
cp -R _out_/* release/
mv release/gems/* release
rmdir release/gems
rm release/website/website.tar.bz2
rmdir release/website
rm release/webasm/sorbet-wasm.tar
rmdir release/webasm

pushd release
files=()
while IFS='' read -r line; do files+=("$line"); done < <(find . -type f | sed 's#^./##')
release_notes="To use Sorbet add this line to your Gemfile:
\`\`\`
gem 'sorbet', '$prefix.$git_commit_count'
\`\`\`
Changelog:

$changes
"
if [ "$dryrun" = "" ]; then
    echo "$release_notes" | ../.buildkite/tools/gh-release.sh sorbet/sorbet "${long_release_version}" -- "${files[@]}"
fi
popd
