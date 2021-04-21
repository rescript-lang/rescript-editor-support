function exp {
  echo "$(dirname $1)/expected/$(basename $1).txt"
}

for file in tests/src/*.res; do
  lib/rescript-editor-support.exe test $file &> $(exp $file)
done
