function exp {
  echo "$(dirname $1)/expected/$(basename $1).txt"
}

for file in tests/src/*.res; do
  lib/rescript-editor-support.exe test $file &> $(exp $file)
done

diff=$(git ls-files --modified tests/)
if [[ $diff = "" ]]; then
  printf "${successGreen}✅ No unstaged tests difference.${reset}\n"
else
  printf "${warningYellow}⚠️ There are unstaged differences in tests/! Did you break a test?\n${diff}\n${reset}"
  git diff
  exit 1
fi
