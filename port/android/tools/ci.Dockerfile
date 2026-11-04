# A clean build machine like CI's (Ubuntu 24.04 with tools/ci-packages.txt), to test tools/release.sh locally:
#   docker build -t totk-ci -f port/android/tools/ci.Dockerfile port/android/tools
#   (the Android SDK is mounted at /opt/android-sdk)
FROM ubuntu:24.04
COPY ci-packages.txt /tmp/
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $(cat /tmp/ci-packages.txt) && rm -rf /var/lib/apt/lists/*
