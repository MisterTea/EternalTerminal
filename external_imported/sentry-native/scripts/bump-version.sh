#!/usr/bin/env bash
set -eux

if [ "$(uname -s)" != "Linux" ]; then
    echo "Please use the GitHub Action."
    exit 1
fi

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd $SCRIPT_DIR/..

OLD_VERSION="$1"
NEW_VERSION="$2"

echo "Bumping version: ${NEW_VERSION}"

perl -pi -e "s/^#define SENTRY_SDK_VERSION.*/#define SENTRY_SDK_VERSION \"${NEW_VERSION}\"/" include/sentry.h
perl -pi -e "s/\"version\": \"[^\"]+\"/\"version\": \"${NEW_VERSION}\"/" tests/assertions.py
perl -pi -e "s/sentry.native\/[^\"]+\"/sentry.native\/${NEW_VERSION}\"/" tests/test_integration_http.py
