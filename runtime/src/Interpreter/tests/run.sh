if [[ ! -v CJNATIVE_TOOLCHAIN ]]; then
    echo "Error: variable CJNATIVE_TOOLCHAIN is unset."
    exit 1
fi
source $CJNATIVE_TOOLCHAIN/envsetup.sh

if [ -z "$1" ]; then
    echo "Error: no test provided."
    echo "Usage: $0 <test_name>"
    exit 1
fi
TEST_NAME="$1"

export INTERPRETER_ENABLED=true
export INTERPRETER_LIB_NAME="libinterpreter.so"
export LD_LIBRARY_PATH=./$TEST_NAME/out:$LD_LIBRARY_PATH

export cjHeapSize=256mb

# export MRT_LOG_DEBUG=1
# export MRT_LOG_EXCEPTION=1

./$TEST_NAME/out/test
exit
