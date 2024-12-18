#!/usr/bin/env bash

echo "Custom fscrypt CLI setup begin"

set -xe

zgrep -h ENCRYPTION /proc/config.gz /boot/config-$(uname -r) | sort | uniq
cat /proc/mounts

# Update the package manager cache
sudo apt-get update -y || sudo yum makecachework    

# Remove any pre-installed fscrypt packages
sudo apt-get remove --purge -y fscrypt || sudo yum remove -y fscrypt || true
sudo apt-get autoremove -y || true

# Install required dependencies for building fscrypt
sudo apt-get install -y build-essential libpam0g-dev libtinfo-dev golang || sudo yum install -y gcc make pam-devel ncurses-devel golang

# Clone the custom fscrypt repository, build and install
git clone https://github.com/chrisphoffman/fscrypt.git -b wip-ceph-fuse /tmp/fscrypt
cd /tmp/fscrypt
make
sudo make install PREFIX=/usr/local

# Add the installed binary path to the PATH environment variable
export PATH=/usr/local/bin:$PATH


echo "Custom fscrypt CLI setup done"