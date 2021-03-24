rm -Rf external_imported
git clean -xfd
git submodule foreach --recursive git clean -xfd
git submodule foreach --recursive git reset --hard
git submodule update --init --recursive
cp -Rf external external_imported
find external_imported | grep "\.git$" | xargs rm -Rf
git add -f external_imported
