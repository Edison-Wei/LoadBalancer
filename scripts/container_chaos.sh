#!/bin/sh
# The host supplies a writable /evidence directory. Preserve failed runs too.
set +e
ctest --preset release -L chaos --output-junit /evidence/chaos.xml > /evidence/ctest.log 2>&1
result=$?
cp -R build/release/Testing /evidence/Testing
cat /evidence/ctest.log
exit "$result"
