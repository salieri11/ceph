#!/usr/bin/env bash

set -xe

mydir=`dirname $0`

if [ $# -ne 2 ]
then
	echo "2 parameters are required!\n"
	echo "Usage:"
	echo "  fscrypt.sh <type> <testdir>"
	echo "  type: should be any of 'none', 'unlocked' or 'locked'"
	echo "  testdir: the test direcotry name"
	exit 1
fi

fscrypt=$1
testcase=$2
testdir=fscrypt_test_${fscrypt}_${testcase}
mkdir $testdir

# Path to the custom fscrypt CLI
FSCRYPT_CLI="$(type -P fscrypt)"

if [ -z "$FSCRYPT_CLI" ]; then
    echo "fscrypt CLI not found. Ensure it is installed and in your PATH."
    exit 1
fi

install_deps()
{
	local system_value=$(sudo lsb_release -is | awk '{print tolower($0)}')
	case $system_value in
		"centos" | "centosstream" | "fedora")
			sudo yum install -y inih-devel userspace-rcu-devel \
				libblkid-devel gettext libedit-devel \
				libattr-devel device-mapper-devel libicu-devel
			;;
		"ubuntu" | "debian")
			sudo apt-get install -y libinih-dev liburcu-dev \
				libblkid-dev gettext libedit-dev libattr1-dev \
				libdevmapper-dev libicu-dev pkg-config
			;;
		*)
			echo "Unsupported distro $system_value"
			exit 1
			;;
	esac
}

clean_up()
{
    rm -rf $testdir
}

# For now will test the V2 encryption policy only as the
# V1 encryption policy is deprecated

# Initialize fscrypt on the filesystem containing the test directory
$FSCRYPT_CLI setup $testdir

# Generate a fixed keying identifier
key_name="test-key"
$FSCRYPT_CLI metadata key create --name="$key_name"
key_descriptor=$($FSCRYPT_CLI metadata key describe --name="$key_name" | awk '{print $NF}')

case ${fscrypt} in
	"none")
		# do nothing for the test directory and will test it
		# as one non-encrypted directory.
		pushd $testdir
		${mydir}/../suites/${testcase}.sh
		popd
		clean_up
		;;
	"unlocked")
		# set encrypt policy with the key provided and then
		# the test directory will be encrypted & unlocked
		$FSCRYPT_CLI encrypt $testdir --key=$key_name
		pushd $testdir
		${mydir}/../suites/${testcase}.sh
		popd
		clean_up
		;;
	"locked")
		# remove the key, then the test directory will be locked
		# and any modification will be denied by requiring the key
		$FSCRYPT_CLI lock $testdir
		clean_up
		;;
	*)
		clean_up
		echo "Unknown parameter $1"
		exit 1
esac
