git log --format="* %cd %aN%n- (%h) %s%d%n" --date=local | gsed -r 's/[0-9]+:[0-9]+:[0-9]+ //'
