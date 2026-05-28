python3 build.py clean
python3 build.py build -t debug --target-lib $PWD/../runtime/output 2>&1 | tee logs.log
python3 build.py install --prefix $PWD/../../cangjie_compiler/output