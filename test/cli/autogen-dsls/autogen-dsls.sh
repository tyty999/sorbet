#!/bin/bash
set -eu

echo "--- autogen-dsls ---"
main/sorbet --silence-dev-message --stop-after=namer -p autogen-dsls \
  test/cli/autogen-dsls/dsls.rb
